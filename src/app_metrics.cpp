#include "app_metrics.h"
#include "core_api.h"

namespace {

std::string serialize_metrics_window(const spp::sparse_hash_map<std::string, uint64_t>& counts,
                                     const spp::sparse_hash_map<std::string, TDigest>& durations,
                                     const std::string& rps_key,
                                     const std::string& latency_key) {
    nlohmann::json result;

    uint64_t total_counts = 0;
    auto MIN = "min_";
    auto MAX = "max_";
    auto PERCENTILE70 = "70Percentile_";
    auto PERCENTILE95 = "95Percentile_";
    auto PERCENTILE99 = "99Percentile_";
    auto SEARCH_RPS_KEY = AppMetrics::SEARCH_LABEL + "_" + rps_key;
    auto SEARCH_LATENCY_KEY = AppMetrics::SEARCH_LABEL + "_" + latency_key;
    auto SEARCH_LATENCY_MIN_KEY = AppMetrics::SEARCH_LABEL + "_" + MIN + latency_key;
    auto SEARCH_LATENCY_MAX_KEY = AppMetrics::SEARCH_LABEL + "_" + MAX + latency_key;
    auto SEARCH_LATENCY_70PERCENTILE_KEY = AppMetrics::SEARCH_LABEL + "_" + PERCENTILE70 + latency_key;
    auto SEARCH_LATENCY_95PERCENTILE_KEY = AppMetrics::SEARCH_LABEL + "_" + PERCENTILE95 + latency_key;
    auto SEARCH_LATENCY_99PERCENTILE_KEY = AppMetrics::SEARCH_LABEL + "_" + PERCENTILE99 + latency_key;

    auto IMPORT_RPS_KEY = AppMetrics::IMPORT_LABEL + "_" + rps_key;
    auto IMPORT_LATENCY_KEY = AppMetrics::IMPORT_LABEL + "_" + latency_key;
    auto IMPORT_LATENCY_MIN_KEY = AppMetrics::IMPORT_LABEL + "_" + MIN + latency_key;
    auto IMPORT_LATENCY_MAX_KEY = AppMetrics::IMPORT_LABEL + "_" + MAX + latency_key;
    auto IMPORT_LATENCY_70PERCENTILE_KEY = AppMetrics::IMPORT_LABEL + "_" + PERCENTILE70 + latency_key;
    auto IMPORT_LATENCY_95PERCENTILE_KEY = AppMetrics::IMPORT_LABEL + "_" + PERCENTILE95 + latency_key;
    auto IMPORT_LATENCY_99PERCENTILE_KEY = AppMetrics::IMPORT_LABEL + "_" + PERCENTILE99 + latency_key;

    auto DOC_WRITE_RPS_KEY = AppMetrics::DOC_WRITE_LABEL + "_" + rps_key;
    auto DOC_WRITE_LATENCY_KEY = AppMetrics::DOC_WRITE_LABEL + "_" + latency_key;
    auto DOC_WRITE_LATENCY_MIN_KEY = AppMetrics::DOC_WRITE_LABEL + "_" + MIN + latency_key;
    auto DOC_WRITE_LATENCY_MAX_KEY = AppMetrics::DOC_WRITE_LABEL + "_" + MAX + latency_key;
    auto DOC_WRITE_LATENCY_70PERCENTILE_KEY = AppMetrics::DOC_WRITE_LABEL + "_" + PERCENTILE70 + latency_key;
    auto DOC_WRITE_LATENCY_95PERCENTILE_KEY = AppMetrics::DOC_WRITE_LABEL + "_" + PERCENTILE95 + latency_key;
    auto DOC_WRITE_LATENCY_99PERCENTILE_KEY = AppMetrics::DOC_WRITE_LABEL + "_" + PERCENTILE99 + latency_key;

    auto DOC_DELETE_RPS_KEY = AppMetrics::DOC_DELETE_LABEL + "_" + rps_key;
    auto DOC_DELETE_LATENCY_KEY = AppMetrics::DOC_DELETE_LABEL + "_" + latency_key;
    auto DOC_DELETE_LATENCY_MIN_KEY = AppMetrics::DOC_DELETE_LABEL + "_" + MIN + latency_key;
    auto DOC_DELETE_LATENCY_MAX_KEY = AppMetrics::DOC_DELETE_LABEL + "_" + MAX + latency_key;
    auto DOC_DELETE_LATENCY_70PERCENTILE_KEY = AppMetrics::DOC_DELETE_LABEL + "_" + PERCENTILE70 + latency_key;
    auto DOC_DELETE_LATENCY_95PERCENTILE_KEY = AppMetrics::DOC_DELETE_LABEL + "_" + PERCENTILE95 + latency_key;
    auto DOC_DELETE_LATENCY_99PERCENTILE_KEY = AppMetrics::DOC_DELETE_LABEL + "_" + PERCENTILE99 + latency_key;

    auto CACHE_HIT_COUNT_KEY = AppMetrics::CACHE_HIT_LABEL + "_" + "count";
    auto CACHE_MISS_COUNT_KEY = AppMetrics::CACHE_MISS_LABEL + "_" + "count";
    auto CACHE_HIT_RATIO_KEY = AppMetrics::CACHE_HIT_LABEL + "_" + "ratio";

    auto OVERLOADED_RPS_KEY = AppMetrics::OVERLOADED_LABEL + "_" + rps_key;

    result[rps_key] = nlohmann::json::object();
    for(const auto& kv: counts) {
        if(kv.first == AppMetrics::SEARCH_LABEL) {
            result[SEARCH_RPS_KEY] = double(kv.second) / (AppMetrics::METRICS_REFRESH_INTERVAL_MS / 1000);
        } else if(kv.first == AppMetrics::IMPORT_LABEL) {
            result[IMPORT_RPS_KEY] = double(kv.second) / (AppMetrics::METRICS_REFRESH_INTERVAL_MS / 1000);
        } else if(kv.first == AppMetrics::DOC_WRITE_LABEL) {
            result[DOC_WRITE_RPS_KEY] = double(kv.second) / (AppMetrics::METRICS_REFRESH_INTERVAL_MS / 1000);
        } else if(kv.first == AppMetrics::DOC_DELETE_LABEL) {
            result[DOC_DELETE_RPS_KEY] = double(kv.second) / (AppMetrics::METRICS_REFRESH_INTERVAL_MS / 1000);
        } else if(kv.first == AppMetrics::OVERLOADED_LABEL) {
            result[OVERLOADED_RPS_KEY] = double(kv.second) / (AppMetrics::METRICS_REFRESH_INTERVAL_MS / 1000);
        } else if(kv.first == AppMetrics::CACHE_HIT_LABEL) {
            result[CACHE_HIT_COUNT_KEY] = kv.second;
        } else if(kv.first == AppMetrics::CACHE_MISS_LABEL) {
            result[CACHE_MISS_COUNT_KEY] = kv.second;
        } else {
            result[rps_key][kv.first] = (double(kv.second) / (AppMetrics::METRICS_REFRESH_INTERVAL_MS / 1000));
            total_counts += kv.second;
        }
    }

    if(counts.find(AppMetrics::CACHE_HIT_LABEL) == counts.end() || counts.find(AppMetrics::CACHE_MISS_LABEL) == counts.end()) {
        result[CACHE_HIT_RATIO_KEY] = 0.0;
    } else if(counts.find(AppMetrics::CACHE_HIT_LABEL)->second == 0) {
        result[CACHE_HIT_RATIO_KEY] = 0.0;
    } else if(counts.find(AppMetrics::CACHE_MISS_LABEL)->second == 0) {
        result[CACHE_HIT_RATIO_KEY] = 1.0;
    } else {
        double cache_hit_val = counts.find(AppMetrics::CACHE_HIT_LABEL)->second;
        double cache_miss_val = counts.find(AppMetrics::CACHE_MISS_LABEL)->second;
        result[CACHE_HIT_RATIO_KEY] = cache_hit_val / (cache_hit_val + cache_miss_val);
    }

    result["total_" + rps_key] = double(total_counts) / (AppMetrics::METRICS_REFRESH_INTERVAL_MS / 1000);
    result[latency_key] = nlohmann::json::object();

    for(const auto& kv: durations) {
        auto counter_it = counts.find(kv.first);
        if(counter_it != counts.end() && counter_it->second != 0) {
            auto digest = kv.second;
            auto total_duration = kv.second.sum();

            if(kv.first == AppMetrics::SEARCH_LABEL) {
                result[SEARCH_LATENCY_KEY] = (double(total_duration) / counter_it->second);
                result[SEARCH_LATENCY_MIN_KEY] = kv.second.min();
                result[SEARCH_LATENCY_MAX_KEY] = kv.second.max();
                result[SEARCH_LATENCY_70PERCENTILE_KEY] = digest.percentile(70);
                result[SEARCH_LATENCY_95PERCENTILE_KEY] = digest.percentile(95);
                result[SEARCH_LATENCY_99PERCENTILE_KEY] = digest.percentile(99);
            } else if(kv.first == AppMetrics::IMPORT_LABEL) {
                result[IMPORT_LATENCY_KEY] = (double(total_duration) / counter_it->second);
                result[IMPORT_LATENCY_MIN_KEY] = kv.second.min();
                result[IMPORT_LATENCY_MAX_KEY] = kv.second.max();
                result[IMPORT_LATENCY_70PERCENTILE_KEY] = digest.percentile(70);
                result[IMPORT_LATENCY_95PERCENTILE_KEY] = digest.percentile(95);
                result[IMPORT_LATENCY_99PERCENTILE_KEY] = digest.percentile(99);
            } else if(kv.first == AppMetrics::DOC_WRITE_LABEL) {
                result[DOC_WRITE_LATENCY_KEY] = (double(total_duration) / counter_it->second);
                result[DOC_WRITE_LATENCY_MIN_KEY] = kv.second.min();
                result[DOC_WRITE_LATENCY_MAX_KEY] = kv.second.max();
                result[DOC_WRITE_LATENCY_70PERCENTILE_KEY] = digest.percentile(70);
                result[DOC_WRITE_LATENCY_95PERCENTILE_KEY] = digest.percentile(95);
                result[DOC_WRITE_LATENCY_99PERCENTILE_KEY] = digest.percentile(99);
            } else if(kv.first == AppMetrics::DOC_DELETE_LABEL) {
                result[DOC_DELETE_LATENCY_KEY] = (double(total_duration) / counter_it->second);
                result[DOC_DELETE_LATENCY_MIN_KEY] = kv.second.min();
                result[DOC_DELETE_LATENCY_MAX_KEY] = kv.second.max();
                result[DOC_DELETE_LATENCY_70PERCENTILE_KEY] = digest.percentile(70);
                result[DOC_DELETE_LATENCY_95PERCENTILE_KEY] = digest.percentile(95);
                result[DOC_DELETE_LATENCY_99PERCENTILE_KEY] = digest.percentile(99);
            } else {
                result[latency_key][kv.first] = (double(total_duration) / counter_it->second);
            }
        }
    }

    std::vector<std::string> keys_to_check = {
        SEARCH_RPS_KEY, IMPORT_RPS_KEY, DOC_WRITE_RPS_KEY, DOC_DELETE_RPS_KEY,
        SEARCH_LATENCY_KEY, IMPORT_LATENCY_KEY, DOC_WRITE_LATENCY_KEY, DOC_DELETE_LATENCY_KEY,
        OVERLOADED_RPS_KEY
    };

    for(auto& key: keys_to_check) {
        if(!result.contains(key)) {
            result[key] = 0;
        }
    }

    return result.dump(2);
}

}

void AppMetrics::increment_write_metrics(uint64_t route_hash, uint64_t duration) {
    if(is_doc_import_route(route_hash)) {
        AppMetrics::get_instance().increment_duration(AppMetrics::IMPORT_LABEL, duration);
        AppMetrics::get_instance().increment_count(AppMetrics::IMPORT_LABEL, 1);
    }

    else if(is_doc_write_route(route_hash)) {
        AppMetrics::get_instance().increment_duration(AppMetrics::DOC_WRITE_LABEL, duration);
        AppMetrics::get_instance().increment_count(AppMetrics::DOC_WRITE_LABEL, 1);
    }

    else if(is_doc_del_route(route_hash)) {
        AppMetrics::get_instance().increment_duration(AppMetrics::DOC_DELETE_LABEL, duration);
        AppMetrics::get_instance().increment_count(AppMetrics::DOC_DELETE_LABEL, 1);
    }
}

void AppMetrics::get(const std::string& rps_key, const std::string& latency_key, nlohmann::json& result) const {
    std::string serialized;
    {
        std::shared_lock lock(mutex);
        serialized = serialized_window_snapshot;
    }
    result = nlohmann::json::parse(serialized.empty() ? "{}" : serialized);
}

void AppMetrics::window_reset() {
    spp::sparse_hash_map<std::string, uint64_t>* previous_counts = nullptr;
    spp::sparse_hash_map<std::string, TDigest>* previous_durations = nullptr;
    spp::sparse_hash_map<std::string, uint64_t>* snapshot_counts = nullptr;
    spp::sparse_hash_map<std::string, TDigest>* snapshot_durations = nullptr;

    {
        std::unique_lock lock(mutex);
        previous_counts = counts;
        counts = current_counts;
        current_counts = new spp::sparse_hash_map<std::string, uint64_t>();

        previous_durations = durations;
        durations = current_durations;
        current_durations = new spp::sparse_hash_map<std::string, TDigest>();

        snapshot_counts = counts;
        snapshot_durations = durations;
    }

    const std::string serialized = serialize_metrics_window(*snapshot_counts, *snapshot_durations,
                                                            "requests_per_second", "latency_ms");
    {
        std::unique_lock lock(mutex);
        serialized_window_snapshot = serialized;
    }

    delete previous_counts;
    delete previous_durations;
}

void AppMetrics::write_access_log(const uint64_t epoch_millis, const char* remote_ip, const std::string& path) {
    if(!access_log_path.empty()) {
        access_log << epoch_millis << "\t" << remote_ip << "\t" << path << "\n";
    }
}

void AppMetrics::flush_access_log() {
    if(!access_log_path.empty()) {
        access_log << std::flush;
    }
}
