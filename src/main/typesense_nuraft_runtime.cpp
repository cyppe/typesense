#include <algorithm>
#include <filesystem>
#include <iostream>
#include <thread>
#include <string>

#include <cmdline.h>
#include <curl/curl.h>

#include "analytics_manager.h"
#include "collection_manager.h"
#include "conversation_model_manager.h"
#include "core_api.h"
#include "curation_index_manager.h"
#include "http_client.h"
#include "nuraft/nuraft_http_runtime.h"
#include "nuraft/nuraft_runtime_options.h"
#include "natural_language_search_model_manager.h"
#include "personalization_model_manager.h"
#include "ratelimit_manager.h"
#include "stemmer_manager.h"
#include "store.h"
#include "stopwords_manager.h"
#include "synonym_index_manager.h"
#include "threadpool.h"
#include "typesense_server_utils.h"
#include "tsconfig.h"

namespace {

#define TS_STRINGIFY_IMPL(x) #x
#define TS_STRINGIFY(x) TS_STRINGIFY_IMPL(x)

}  // namespace

int main(int argc, char** argv) {
    NuRaftHttpServerOptions options;
    cmdline::parser cmdline_options;
    bool help_requested = false;
    std::string error;
    std::string usage;
    Config& config = Config::get_instance();
    if (!load_nuraft_runtime_options(cmdline_options, argc, argv, config, options, help_requested, usage, error)) {
        std::cerr << "Typesense " << TS_STRINGIFY(TYPESENSE_VERSION) << "\n" << error << "\n" << usage;
        return 1;
    }

    if (help_requested) {
        std::cout << usage;
        return 0;
    }

    if (config.get_analytics_dir().empty()) {
        config.set_analytics_dir(options.startup_options.data_dir + "/analytics_db");
    }

    signal(SIGINT, catch_interrupt);
    signal(SIGTERM, catch_interrupt);
    signal(SIGHUP, catch_interrupt);

    const int logger_init = init_root_logger(config, TS_STRINGIFY(TYPESENSE_VERSION));
    if (logger_init != 0) {
        return logger_init;
    }

    curl_global_init(CURL_GLOBAL_SSL);
    HttpClient::get_instance().init(options.api_key);
    init_api(static_cast<uint32_t>(config.get_cache_num_entries()));

    std::atomic<bool> quit_product_state(false);
    const size_t proc_count = std::max<size_t>(1, std::thread::hardware_concurrency());
    const size_t configured_thread_pool_size = config.get_thread_pool_size();
    const size_t num_threads = configured_thread_pool_size == 0 ? (proc_count * 8) : configured_thread_pool_size;
    const size_t configured_parallel_collection_load = config.get_num_collections_parallel_load();
    const size_t num_collections_parallel_load =
        configured_parallel_collection_load == 0 ? (proc_count * 4) : configured_parallel_collection_load;

    ThreadPool app_thread_pool(num_threads);
    ThreadPool server_thread_pool(num_threads);
    std::filesystem::create_directories(options.startup_options.data_dir);
    std::filesystem::create_directories(options.startup_options.data_dir + "/db");
    std::filesystem::create_directories(config.get_analytics_dir());
    std::filesystem::create_directories(config.get_analytics_dir() + "/db");
    Store store(options.startup_options.data_dir + "/db",
                24 * 60 * 60,
                1024,
                true,
                0,
                config.get_db_write_buffer_size(),
                config.get_db_max_write_buffer_number(),
                config.get_db_max_log_file_size(),
                config.get_db_keep_log_file_num(),
                config.get_db_block_cache_size(),
                config.get_db_rate_limit_bytes_per_sec(),
                config.get_db_level_compaction_dynamic_level_bytes(),
                static_cast<uint32_t>(config.get_db_block_size()),
                static_cast<uint32_t>(config.get_db_format_version()),
                config.get_db_enable_statistics(),
                static_cast<uint32_t>(config.get_db_compression_parallel_threads()),
                static_cast<uint64_t>(config.get_db_bytes_per_sync()),
                static_cast<uint64_t>(config.get_db_max_manifest_file_size()),
                config.get_db_enable_async_io(),
                config.get_db_offpeak_time_utc(),
                config.get_db_unordered_write(),
                static_cast<uint32_t>(config.get_db_max_subcompactions()),
                static_cast<uint32_t>(config.get_db_max_background_jobs()),
                config.get_db_use_direct_reads(),
                config.get_db_use_direct_io_for_flush_and_compaction(),
                static_cast<uint64_t>(config.get_db_compaction_readahead_size()),
                config.get_db_optimize_filters_for_hits(),
                config.get_db_paranoid_memory_checks());
    auto analytics_store = std::make_unique<Store>(config.get_analytics_dir() + "/db",
                                                   24 * 60 * 60,
                                                   1024,
                                                   true,
                                                   config.get_analytics_db_ttl());
    HttpServer http_server(
        TS_STRINGIFY(TYPESENSE_VERSION),
        config.get_api_address(),
        static_cast<uint32_t>(config.get_api_port()),
        config.get_ssl_cert(),
        config.get_ssl_cert_key(),
        config.get_ssl_refresh_interval_seconds() * 1000,
        config.get_request_timeout_ms(),
        config.get_enable_cors(),
        config.get_cors_domains(),
        &server_thread_pool
    );
    server = &http_server;
    http_server.set_auth_handler(nuraft_http_runtime_auth);
    http_server.on(HttpServer::STREAM_RESPONSE_MESSAGE, HttpServer::on_stream_response_message);
    http_server.on(HttpServer::REQUEST_PROCEED_MESSAGE, HttpServer::on_request_proceed_message);
    http_server.on(HttpServer::DEFER_PROCESSING_MESSAGE, HttpServer::on_deferred_processing_message);
    register_nuraft_http_runtime_routes(&http_server);

    CollectionManager& collection_manager = CollectionManager::get_instance();
    collection_manager.init(&store,
                            &app_thread_pool,
                            config.get_max_memory_ratio(),
                            config.get_api_key(),
                            quit_product_state,
                            config.get_filter_by_max_ops());
    StopwordsManager::get_instance().init(&store);
    StemmerManager::get_instance().init(&store);
    SynonymIndexManager::get_instance().init_store(&store);
    CurationIndexManager::get_instance().init_store(&store);
    AnalyticsManager::get_instance().init(&store,
                                          analytics_store.get(),
                                          static_cast<uint32_t>(config.get_analytics_minute_rate_limit()));
    const auto rate_limit_manager_init = RateLimitManager::getInstance()->init(&store);
    if (!rate_limit_manager_init.ok()) {
        TS_LOG(INFO) << "NuRaft runtime failed to initialize rate limit manager: "
                     << rate_limit_manager_init.error();
    }
    static_cast<void>(ConversationModelManager::init(&store));
    static_cast<void>(PersonalizationModelManager::init(&store));
    static_cast<void>(NaturalLanguageSearchModelManager::init(&store));

    const auto load_op = collection_manager.load(num_collections_parallel_load,
                                                 config.get_num_documents_parallel_load());
    if (!load_op.ok()) {
        collection_manager.dispose();
        app_thread_pool.shutdown();
        std::cerr << load_op.error() << "\n";
        curl_global_cleanup();
        return 1;
    }

    NuRaftHttpRuntimeService runtime_service(&http_server, options);
    if (!runtime_service.initialize(error)) {
        AnalyticsManager::get_instance().dispose();
        NaturalLanguageSearchModelManager::dispose();
        PersonalizationModelManager::dispose();
        CurationIndexManager::get_instance().dispose();
        SynonymIndexManager::get_instance().dispose();
        StemmerManager::get_instance().dispose();
        StopwordsManager::get_instance().dispose();
        collection_manager.dispose();
        app_thread_pool.shutdown();
        std::cerr << error << "\n";
        curl_global_cleanup();
        return 1;
    }

    std::thread analytics_thread([&runtime_service]() {
        AnalyticsManager::get_instance().run(&runtime_service);
    });
    const int exit_code = http_server.run(&runtime_service);
    quit_product_state.store(true);
    server = nullptr;
    AnalyticsManager::get_instance().stop();
    if (analytics_thread.joinable()) {
        analytics_thread.join();
    }
    NaturalLanguageSearchModelManager::dispose();
    PersonalizationModelManager::dispose();
    CurationIndexManager::get_instance().dispose();
    SynonymIndexManager::get_instance().dispose();
    StemmerManager::get_instance().dispose();
    StopwordsManager::get_instance().dispose();
    collection_manager.dispose();
    app_thread_pool.shutdown();
    server_thread_pool.shutdown();
    curl_global_cleanup();
    return exit_code;
}
