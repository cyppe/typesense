#include "typesense_server_utils.h"

#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log_sink_registry.h"
#include "file_utils.h"
#include "logger.h"
#include "ts_log_sink.h"

HttpServer* server = nullptr;
std::atomic<bool> quit_raft_service = false;

void catch_interrupt(int sig) {
    TS_LOG(INFO) << "Stopping Typesense server...";
    if (sig == SIGHUP && server != nullptr) {
        TS_LOG(INFO) << "shutdown is triggered.";
        server->set_shutdown_triggered();
        const auto secs = Config::get_instance().get_shutdown_delay_seconds();
        std::thread shutdown_thread([secs]() {
            std::this_thread::sleep_for(std::chrono::seconds(secs));
            quit_raft_service.store(true);
        });
        shutdown_thread.detach();
    } else {
        quit_raft_service.store(true);
    }

    signal(sig, SIG_IGN);
}

void init_cmdline_options(cmdline::parser& options, int argc, char** argv) {
    static_cast<void>(argc);
    static_cast<void>(argv);

    options.set_program_name("./typesense-server");

    options.add<std::string>("data-dir", 'd', "Directory where data will be stored.", false);
    options.add<std::string>("api-key", 'a', "API key that allows all operations.", false);
    options.add<std::string>("search-only-api-key", 's', "[DEPRECATED: use API key management end-point] API key that allows only searches.", false);
    options.add<std::string>("health-rusage-api-key", '\0', "API key that allows access to health end-point with resource usage.", false);
    options.add<std::string>("analytics-dir", '\0', "Directory where Analytics will be stored.", false);
    options.add<uint32_t>("analytics-db-ttl", '\0', "TTL in seconds for events stored in analytics db", false, 2419200);
    options.add<uint32_t>("analytics-minute-rate-limit", '\0', "per minute rate limit for /events endpoint", false, 5);
    options.add<uint32_t>("shutdown-delay-seconds", '\0', "delay in seconds after which server will shutdown on receiving signal", false, 0);

    options.add<std::string>("api-address", '\0', "Address to which Typesense API service binds.", false, "0.0.0.0");
    options.add<uint32_t>("api-port", '\0', "Port on which Typesense API service listens.", false, 8108);
    options.add<uint32_t>("request-timeout-ms", '\0',
                          "HTTP request timeout in milliseconds for HTTP/1 request/read and HTTP/2 idle timeouts.",
                          false, 60000);

    options.add<std::string>("peering-address", '\0', "Internal IP address to which Typesense peering service binds.", false, "");
    options.add<uint32_t>("peering-port", '\0', "Port on which Typesense peering service listens.", false, 8107);
    options.add<std::string>("peering-subnet", '\0', "Internal subnet that Typesense should use for peering.", false, "");
    options.add<std::string>("nodes", '\0', "Path to file containing comma separated string of all nodes in the cluster.", false);

    options.add<std::string>("ssl-certificate", 'c', "Path to the SSL certificate file.", false, "");
    options.add<std::string>("ssl-certificate-key", 'k', "Path to the SSL certificate key file.", false, "");
    options.add<uint32_t>("ssl-refresh-interval-seconds", '\0', "Frequency of automatic reloading of SSL certs from disk.", false, 8 * 60 * 60);

    options.add<bool>("enable-cors", '\0', "Enable CORS requests.", false, true);
    options.add<std::string>("cors-domains", '\0', "Comma separated list of domains that are allowed for CORS.", false, "");

    options.add<float>("max-memory-ratio", '\0', "Maximum fraction of system memory to be used.", false, 1.0f);
    options.add<int>("snapshot-interval-seconds", '\0', "Frequency of replication log snapshots.", false, 3600);
    options.add<int>("snapshot-max-byte-count-per-rpc", '\0', "Maximum snapshot file size in bytes transferred for each RPC.", false, 4194304);
    options.add<size_t>("healthy-read-lag", '\0', "Reads are rejected if the updates lag behind this threshold.", false, 1000);
    options.add<size_t>("healthy-write-lag", '\0', "Writes are rejected if the updates lag behind this threshold.", false, 500);
    options.add<int>("log-slow-requests-time-ms", '\0', "When >= 0, requests that take longer than this duration are logged.", false, -1);

    options.add<uint32_t>("num-collections-parallel-load", '\0', "Number of collections that are loaded in parallel during start up. Use 0 for dynamic sizing (NUM_CORES * 4).", false, 0);
    options.add<uint32_t>("num-documents-parallel-load", '\0', "Number of documents per collection that are indexed in parallel during start up.", false, 1000);

    options.add<uint32_t>("thread-pool-size", '\0', "Number of threads used for handling concurrent requests. Use 0 for dynamic sizing (NUM_CORES * 8).", false, 0);

    options.add<std::string>("log-dir", '\0', "Path to the log directory.", false, "");
    options.add<std::string>("config", '\0', "Path to the configuration file.", false, "");

    options.add<bool>("enable-access-logging", '\0', "Enable access logging.", false, false);
    options.add<bool>("enable-search-logging", '\0', "Enable search logging.", false, false);
    options.add<bool>("enable-search-analytics", '\0', "Enable search analytics.", false, false);
    options.add<int>("disk-used-max-percentage", '\0', "Reject writes when used disk space exceeds this percentage. Default: 100 (never reject).", false, 100);
    options.add<int>("memory-used-max-percentage", '\0', "Reject writes when memory usage exceeds this percentage. Default: 100 (never reject).", false, 100);
    options.add<bool>("skip-writes", '\0', "Skip all writes except config changes. Default: false.", false, false);
    options.add<bool>("reset-peers-on-error", '\0', "Reset node's peers on clustering error. Default: false.", false, false);

    options.add<int>("log-slow-searches-time-ms", '\0', "When >= 0, searches that take longer than this duration are logged.", false, 30 * 1000);
    options.add<uint32_t>("cache-num-entries", '\0', "Number of entries to cache.", false, 1000);
    options.add<uint32_t>("embedding-cache-num-entries", '\0', "Number of entries to cache for embeddings.", false, 100);
    options.add<uint32_t>("analytics-flush-interval", '\0', "Frequency of persisting analytics data to disk (in seconds).", false, 3600);
    options.add<uint32_t>("housekeeping-interval", '\0', "Frequency of housekeeping background job (in seconds).", false, 1800);
    options.add<bool>("enable-lazy-filter", '\0', "Filter clause will be evaluated lazily.", false, false);
    options.add<uint32_t>("db-compaction-interval", '\0', "Frequency of RocksDB compaction (in seconds).", false, 0);
    options.add<uint16_t>("filter-by-max-ops", '\0', "Maximum number of operations permitted in filtery_by.", false, Config::FILTER_BY_DEFAULT_OPERATIONS);

    options.add<int>("max-per-page", '\0', "Max number of hits per page", false, 250);
    options.add<uint32_t>("max-group-limit", '\0', "Max number of results to be returned per group", false, 99);
    options.add<uint32_t>("max-indexing-concurrency", '\0', "maximum concurrency for batch indexing docs.", false, 4);

    options.add<uint32_t>("proxy-rate-limit", '\0', "proxy rate limit.", false, 1000);
    options.add<std::string>("proxy-disallowed-dest-cidrs", '\0', "Disallowed dest CIDRs for proxy.", false, "");
    options.add<bool>("proxy-allow-only-peer-src-ips", '\0', "Allow only peers as src IPs for proxy.", false, false);

    options.add<uint32_t>("db-write-buffer-size", '\0', "RocksDB write buffer size in bytes.", false, 128 * 1048576);
    options.add<uint32_t>("db-max-write-buffer-number", '\0', "RocksDB max number of write buffers.", false, 4);
    options.add<uint32_t>("db-max-log-file-size", '\0', "RocksDB max log file size in bytes.", false, 4 * 1048576);
    options.add<uint32_t>("db-keep-log-file-num", '\0', "RocksDB number of log files to keep.", false, 5);
    options.add<uint64_t>("db-block-cache-size", '\0', "RocksDB block cache size in bytes.", false, 256ULL * 1048576ULL);
    options.add<int64_t>("db-rate-limit-bytes-per-sec", '\0', "RocksDB rate limiter bytes per second (0 to disable).", false, 0);
    options.add<bool>("db-level-compaction-dynamic-level-bytes", '\0', "RocksDB dynamic level sizing for compaction.", false, true);
    options.add<uint32_t>("db-block-size", '\0', "RocksDB SST block size in bytes.", false, 16 * 1024);
    options.add<uint32_t>("db-format-version", '\0', "RocksDB SST format version (max 7).", false, 7);
    options.add<bool>("db-enable-statistics", '\0', "Enable RocksDB statistics counters.", false, true);
    options.add<uint32_t>("db-compression-parallel-threads", '\0', "RocksDB parallel compression threads.", false, 4);
    options.add<uint64_t>("db-bytes-per-sync", '\0', "RocksDB bytes per sync during writes.", false, 1048576);
    options.add<uint64_t>("db-max-manifest-file-size", '\0', "RocksDB max MANIFEST file size in bytes.", false, 1048576);
    options.add<bool>("db-enable-async-io", '\0', "Enable RocksDB async I/O for iterators.", false, true);
    options.add<std::string>("db-offpeak-time-utc", '\0', "RocksDB off-peak compaction window (HH:MM-HH:MM UTC).", false, "02:00-06:00");
    options.add<bool>("db-unordered-write", '\0', "Enable RocksDB unordered writes for higher throughput (safe when WAL disabled).", false, true);
    options.add<uint32_t>("db-max-subcompactions", '\0', "Max parallel sub-compactions for L0->L1 (0=auto, 1=disabled).", false, 2);
    options.add<uint32_t>("db-max-background-jobs", '\0', "RocksDB max background jobs (0 for auto).", false, 0);
    options.add<bool>("db-use-direct-reads", '\0', "Enable RocksDB O_DIRECT for reads.", false, false);
    options.add<bool>("db-use-direct-io-for-flush-and-compaction", '\0', "Enable RocksDB O_DIRECT for flush and compaction I/O.", false, false);
    options.add<uint64_t>("db-compaction-readahead-size", '\0', "RocksDB compaction readahead size in bytes (0 to disable).", false, 0);
    options.add<bool>("db-optimize-filters-for-hits", '\0', "Optimize RocksDB filters for mostly-positive lookups.", false, false);
    options.add<bool>("db-paranoid-memory-checks", '\0', "Enable RocksDB paranoid memory checks.", false, true);

    options.add<std::string>("listen-address", 'h', "[DEPRECATED: use `api-address`] Address to which Typesense API service binds.", false, "0.0.0.0");
    options.add<uint32_t>("listen-port", 'p', "[DEPRECATED: use `api-port`] Port on which Typesense API service listens.", false, 8108);
    options.add<std::string>("master", 'm', "[DEPRECATED: use clustering via --nodes] Master's address in http(s)://<master_address>:<master_port> format to start as read-only replica.", false, "");
}

static TsFileSink* g_file_sink = nullptr;

int init_root_logger(Config& config, const std::string& server_version) {
    static_cast<void>(server_version);

    absl::InitializeLog();

    const std::string log_dir = config.get_log_dir();
    if (log_dir.empty()) {
        absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);
        return 0;
    }

    if (!directory_exists(log_dir)) {
        std::cerr << "Typesense failed to start. Log directory " << log_dir << " does not exist.";
        return 1;
    }

    const std::string log_path = log_dir + "/typesense.log";
    g_file_sink = new TsFileSink(log_path);
    if (!g_file_sink->ok()) {
        std::cerr << "Typesense failed to start. Could not open log file: " << log_path;
        return 1;
    }

    absl::AddLogSink(g_file_sink);
    absl::SetStderrThreshold(absl::LogSeverityAtLeast::kWarning);
    std::cout << "Log directory is configured as: " << log_dir << std::endl;
    return 0;
}
