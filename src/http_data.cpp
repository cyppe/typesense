#include "http_data.h"

#include <atomic>
#include <mutex>

namespace {

struct http_request_metrics_state_t {
    std::atomic<uint64_t> cumulative_requests{0};
    std::atomic<uint64_t> cumulative_slow_requests{0};
    std::atomic<uint64_t> last_total_ms{0};
    std::atomic<uint64_t> last_auth_ms{0};
    std::atomic<uint64_t> last_handler_wait_ms{0};
    std::atomic<uint64_t> last_handler_ms{0};
    std::atomic<uint64_t> last_unattributed_ms{0};
    std::atomic<uint64_t> last_conn_to_start_ms{0};
    std::atomic<bool> last_is_write{false};
    std::mutex last_route_mutex;
    std::string last_route;
};

http_request_metrics_state_t g_http_request_metrics;

}

std::string route_path::_get_action() {
    // `resource:operation` forms an action
    // operations: create, get, list, delete, search, import, export

    std::string resource_path;
    std::string operation;
    size_t identifier_index = 0;

    for(size_t i = 0; i < path_parts.size(); i++) {
        if(i == 0 && path_parts.size() > 2 && path_parts[i] == "collections") {
            // sub-resource of a collection, e.g. /collections/:name/curations should be treated as
            // top-level resource to maintain backward compatibility
            continue;
        }

        if(path_parts[i][0] == ':') {
            identifier_index = i;
        } else if(resource_path.empty()){
            resource_path = path_parts[i];
        } else {
            resource_path = resource_path + "/" + path_parts[i];
        }
    }

    // special cases to maintain semantics and backward compatibility
    if(resource_path == "multi_search" || resource_path == "documents/search") {
        return "documents:search";
    }

    if(resource_path == "documents/import" || resource_path == "documents/export") {
        StringUtils::replace_all(resource_path, "documents/", "");
        return "documents:" + resource_path;
    }

    if(resource_path == "operations/schema_changes") {
        return "operations/schema_changes:get";
    }

    // e.g /collections or /collections/:collection/foo or /collections/:collection

    if(http_method == "GET") {
        // GET can be a `get` or `list`
        operation = (identifier_index != 0) ? "get" : "list";
    } else if(http_method == "POST") {
        operation = "create";
    } else if(http_method == "PUT") {
        operation = "upsert";
    } else if(http_method == "DELETE") {
        operation = "delete";
    } else if(http_method == "PATCH") {
        operation = "update";
    } else {
        operation = "unknown";
    }

    return resource_path + ":" + operation;
}

bool http_req::do_resource_check() {
    return http_method != "DELETE" && path_without_query != "/health" && path_without_query != "/config";
}

uint64_t http_req::now_ts_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

http_request_metrics_snapshot_t http_req::get_metrics_snapshot() {
    http_request_metrics_snapshot_t snapshot;
    snapshot.cumulative_requests = g_http_request_metrics.cumulative_requests.load(std::memory_order_relaxed);
    snapshot.cumulative_slow_requests = g_http_request_metrics.cumulative_slow_requests.load(std::memory_order_relaxed);
    snapshot.last_total_ms = g_http_request_metrics.last_total_ms.load(std::memory_order_relaxed);
    snapshot.last_auth_ms = g_http_request_metrics.last_auth_ms.load(std::memory_order_relaxed);
    snapshot.last_handler_wait_ms = g_http_request_metrics.last_handler_wait_ms.load(std::memory_order_relaxed);
    snapshot.last_handler_ms = g_http_request_metrics.last_handler_ms.load(std::memory_order_relaxed);
    snapshot.last_unattributed_ms = g_http_request_metrics.last_unattributed_ms.load(std::memory_order_relaxed);
    snapshot.last_conn_to_start_ms = g_http_request_metrics.last_conn_to_start_ms.load(std::memory_order_relaxed);
    snapshot.last_is_write = g_http_request_metrics.last_is_write.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_http_request_metrics.last_route_mutex);
        snapshot.last_route = g_http_request_metrics.last_route;
    }
    return snapshot;
}

void http_req::record_lifecycle_metrics(const http_req& req, const std::string& route, uint64_t total_ms) {
    const auto auth_ms = req.auth_duration_us.load(std::memory_order_relaxed) / 1000;
    const auto handler_dispatch_ts = req.handler_dispatch_ts_us.load(std::memory_order_relaxed);
    const auto handler_start_ts = req.handler_start_ts_us.load(std::memory_order_relaxed);
    const auto handler_end_ts = req.handler_end_ts_us.load(std::memory_order_relaxed);

    uint64_t handler_wait_ms = 0;
    if (handler_dispatch_ts != 0 && handler_start_ts >= handler_dispatch_ts) {
        handler_wait_ms = (handler_start_ts - handler_dispatch_ts) / 1000;
    }

    uint64_t handler_ms = 0;
    if (handler_start_ts != 0 && handler_end_ts >= handler_start_ts) {
        handler_ms = (handler_end_ts - handler_start_ts) / 1000;
    }

    uint64_t conn_to_start_ms = 0;
    if (req.start_ts >= req.conn_ts) {
        conn_to_start_ms = (req.start_ts - req.conn_ts) / 1000;
    }

    uint64_t attributed_ms = auth_ms + handler_wait_ms + handler_ms;
    uint64_t unattributed_ms = total_ms >= attributed_ms ? (total_ms - attributed_ms) : 0;

    g_http_request_metrics.cumulative_requests.fetch_add(1, std::memory_order_relaxed);
    const auto slow_threshold_ms = Config::get_instance().get_log_slow_requests_time_ms();
    if (slow_threshold_ms >= 0 && total_ms >= static_cast<uint64_t>(slow_threshold_ms)) {
        g_http_request_metrics.cumulative_slow_requests.fetch_add(1, std::memory_order_relaxed);
    }
    g_http_request_metrics.last_total_ms.store(total_ms, std::memory_order_relaxed);
    g_http_request_metrics.last_auth_ms.store(auth_ms, std::memory_order_relaxed);
    g_http_request_metrics.last_handler_wait_ms.store(handler_wait_ms, std::memory_order_relaxed);
    g_http_request_metrics.last_handler_ms.store(handler_ms, std::memory_order_relaxed);
    g_http_request_metrics.last_unattributed_ms.store(unattributed_ms, std::memory_order_relaxed);
    g_http_request_metrics.last_conn_to_start_ms.store(conn_to_start_ms, std::memory_order_relaxed);
    g_http_request_metrics.last_is_write.store(req.is_write.load(std::memory_order_relaxed), std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_http_request_metrics.last_route_mutex);
        g_http_request_metrics.last_route = route;
    }
}
