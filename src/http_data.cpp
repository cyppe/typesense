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
    std::atomic<uint64_t> last_response_dispatch_ms{0};
    std::atomic<uint64_t> last_response_queue_ms{0};
    std::atomic<uint64_t> last_response_progress_ms{0};
    std::atomic<uint64_t> last_response_send_calls{0};
    std::atomic<uint64_t> last_response_proceed_count{0};
    std::atomic<uint64_t> last_response_defer_count{0};
    std::atomic<uint64_t> last_response_first_send_delay_ms{0};
    std::atomic<uint64_t> last_response_send_window_ms{0};
    std::atomic<bool> last_response_final_sent{false};
    std::atomic<bool> last_is_write{false};
    std::mutex last_route_mutex;
    std::string last_route;

    std::atomic<uint64_t> import_last_total_ms{0};
    std::atomic<uint64_t> import_last_auth_ms{0};
    std::atomic<uint64_t> import_last_handler_wait_ms{0};
    std::atomic<uint64_t> import_last_handler_ms{0};
    std::atomic<uint64_t> import_last_unattributed_ms{0};
    std::atomic<uint64_t> import_last_conn_to_start_ms{0};
    std::atomic<uint64_t> import_last_response_dispatch_ms{0};
    std::atomic<uint64_t> import_last_response_queue_ms{0};
    std::atomic<uint64_t> import_last_response_progress_ms{0};
    std::atomic<uint64_t> import_last_response_send_calls{0};
    std::atomic<uint64_t> import_last_response_proceed_count{0};
    std::atomic<uint64_t> import_last_response_defer_count{0};
    std::atomic<uint64_t> import_last_response_first_send_delay_ms{0};
    std::atomic<uint64_t> import_last_response_send_window_ms{0};
    std::atomic<bool> import_last_response_final_sent{false};
};

http_request_metrics_state_t g_http_request_metrics;

struct message_dispatch_type_metrics_state_t {
    std::atomic<uint64_t> queued{0};
    std::atomic<uint64_t> cumulative_messages{0};
    std::atomic<uint64_t> last_queue_ms{0};
    std::atomic<uint64_t> max_queue_ms{0};
};

struct message_dispatch_metrics_state_t {
    message_dispatch_type_metrics_state_t stream_response;
    message_dispatch_type_metrics_state_t request_proceed;
    message_dispatch_type_metrics_state_t defer_processing;
    message_dispatch_type_metrics_state_t other;
};

message_dispatch_metrics_state_t g_message_dispatch_metrics;

struct response_flow_metrics_state_t {
    std::atomic<uint64_t> active_deferred_requests{0};
    std::atomic<uint64_t> cumulative_defer_schedules{0};
    std::atomic<uint64_t> cumulative_defer_callbacks{0};
    std::atomic<uint64_t> cumulative_response_proceeds{0};
    std::atomic<uint64_t> cumulative_response_send_calls{0};
    std::atomic<uint64_t> cumulative_response_final_sends{0};
    std::atomic<uint64_t> last_defer_timeout_ms{0};
    std::atomic<uint64_t> last_defer_actual_ms{0};
    std::atomic<uint64_t> max_defer_actual_ms{0};
    std::atomic<uint64_t> last_send_calls_per_request{0};
    std::atomic<uint64_t> last_proceed_count_per_request{0};
    std::atomic<uint64_t> last_defer_count_per_request{0};
    std::atomic<uint64_t> last_first_send_delay_ms{0};
    std::atomic<uint64_t> last_send_window_ms{0};
    std::atomic<bool> last_final_sent{false};
};

response_flow_metrics_state_t g_response_flow_metrics;

message_dispatch_type_metrics_state_t& get_message_dispatch_metrics_state(std::string_view type) {
    if (type == "STREAM_RESPONSE") {
        return g_message_dispatch_metrics.stream_response;
    }

    if (type == "REQUEST_PROCEED") {
        return g_message_dispatch_metrics.request_proceed;
    }

    if (type == "DEFER_PROCESSING") {
        return g_message_dispatch_metrics.defer_processing;
    }

    return g_message_dispatch_metrics.other;
}

message_dispatch_type_metrics_snapshot_t snapshot_message_dispatch_type(
    const message_dispatch_type_metrics_state_t& state) {
    message_dispatch_type_metrics_snapshot_t snapshot;
    snapshot.queued = state.queued.load(std::memory_order_relaxed);
    snapshot.cumulative_messages = state.cumulative_messages.load(std::memory_order_relaxed);
    snapshot.last_queue_ms = state.last_queue_ms.load(std::memory_order_relaxed);
    snapshot.max_queue_ms = state.max_queue_ms.load(std::memory_order_relaxed);
    return snapshot;
}

response_flow_metrics_snapshot_t snapshot_response_flow_metrics() {
    response_flow_metrics_snapshot_t snapshot;
    snapshot.active_deferred_requests = g_response_flow_metrics.active_deferred_requests.load(std::memory_order_relaxed);
    snapshot.cumulative_defer_schedules = g_response_flow_metrics.cumulative_defer_schedules.load(std::memory_order_relaxed);
    snapshot.cumulative_defer_callbacks = g_response_flow_metrics.cumulative_defer_callbacks.load(std::memory_order_relaxed);
    snapshot.cumulative_response_proceeds = g_response_flow_metrics.cumulative_response_proceeds.load(std::memory_order_relaxed);
    snapshot.cumulative_response_send_calls = g_response_flow_metrics.cumulative_response_send_calls.load(std::memory_order_relaxed);
    snapshot.cumulative_response_final_sends = g_response_flow_metrics.cumulative_response_final_sends.load(std::memory_order_relaxed);
    snapshot.last_defer_timeout_ms = g_response_flow_metrics.last_defer_timeout_ms.load(std::memory_order_relaxed);
    snapshot.last_defer_actual_ms = g_response_flow_metrics.last_defer_actual_ms.load(std::memory_order_relaxed);
    snapshot.max_defer_actual_ms = g_response_flow_metrics.max_defer_actual_ms.load(std::memory_order_relaxed);
    snapshot.last_send_calls_per_request = g_response_flow_metrics.last_send_calls_per_request.load(std::memory_order_relaxed);
    snapshot.last_proceed_count_per_request = g_response_flow_metrics.last_proceed_count_per_request.load(std::memory_order_relaxed);
    snapshot.last_defer_count_per_request = g_response_flow_metrics.last_defer_count_per_request.load(std::memory_order_relaxed);
    snapshot.last_first_send_delay_ms = g_response_flow_metrics.last_first_send_delay_ms.load(std::memory_order_relaxed);
    snapshot.last_send_window_ms = g_response_flow_metrics.last_send_window_ms.load(std::memory_order_relaxed);
    snapshot.last_final_sent = g_response_flow_metrics.last_final_sent.load(std::memory_order_relaxed);
    return snapshot;
}

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
    snapshot.last_response_dispatch_ms = g_http_request_metrics.last_response_dispatch_ms.load(std::memory_order_relaxed);
    snapshot.last_response_queue_ms = g_http_request_metrics.last_response_queue_ms.load(std::memory_order_relaxed);
    snapshot.last_response_progress_ms = g_http_request_metrics.last_response_progress_ms.load(std::memory_order_relaxed);
    snapshot.last_response_send_calls = g_http_request_metrics.last_response_send_calls.load(std::memory_order_relaxed);
    snapshot.last_response_proceed_count = g_http_request_metrics.last_response_proceed_count.load(std::memory_order_relaxed);
    snapshot.last_response_defer_count = g_http_request_metrics.last_response_defer_count.load(std::memory_order_relaxed);
    snapshot.last_response_first_send_delay_ms = g_http_request_metrics.last_response_first_send_delay_ms.load(std::memory_order_relaxed);
    snapshot.last_response_send_window_ms = g_http_request_metrics.last_response_send_window_ms.load(std::memory_order_relaxed);
    snapshot.last_response_final_sent = g_http_request_metrics.last_response_final_sent.load(std::memory_order_relaxed);
    snapshot.last_is_write = g_http_request_metrics.last_is_write.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_http_request_metrics.last_route_mutex);
        snapshot.last_route = g_http_request_metrics.last_route;
    }
    snapshot.import_last_total_ms = g_http_request_metrics.import_last_total_ms.load(std::memory_order_relaxed);
    snapshot.import_last_auth_ms = g_http_request_metrics.import_last_auth_ms.load(std::memory_order_relaxed);
    snapshot.import_last_handler_wait_ms = g_http_request_metrics.import_last_handler_wait_ms.load(std::memory_order_relaxed);
    snapshot.import_last_handler_ms = g_http_request_metrics.import_last_handler_ms.load(std::memory_order_relaxed);
    snapshot.import_last_unattributed_ms = g_http_request_metrics.import_last_unattributed_ms.load(std::memory_order_relaxed);
    snapshot.import_last_conn_to_start_ms = g_http_request_metrics.import_last_conn_to_start_ms.load(std::memory_order_relaxed);
    snapshot.import_last_response_dispatch_ms = g_http_request_metrics.import_last_response_dispatch_ms.load(std::memory_order_relaxed);
    snapshot.import_last_response_queue_ms = g_http_request_metrics.import_last_response_queue_ms.load(std::memory_order_relaxed);
    snapshot.import_last_response_progress_ms = g_http_request_metrics.import_last_response_progress_ms.load(std::memory_order_relaxed);
    snapshot.import_last_response_send_calls = g_http_request_metrics.import_last_response_send_calls.load(std::memory_order_relaxed);
    snapshot.import_last_response_proceed_count = g_http_request_metrics.import_last_response_proceed_count.load(std::memory_order_relaxed);
    snapshot.import_last_response_defer_count = g_http_request_metrics.import_last_response_defer_count.load(std::memory_order_relaxed);
    snapshot.import_last_response_first_send_delay_ms =
        g_http_request_metrics.import_last_response_first_send_delay_ms.load(std::memory_order_relaxed);
    snapshot.import_last_response_send_window_ms =
        g_http_request_metrics.import_last_response_send_window_ms.load(std::memory_order_relaxed);
    snapshot.import_last_response_final_sent =
        g_http_request_metrics.import_last_response_final_sent.load(std::memory_order_relaxed);
    return snapshot;
}

message_dispatch_metrics_snapshot_t get_message_dispatch_metrics_snapshot() {
    message_dispatch_metrics_snapshot_t snapshot;
    snapshot.stream_response = snapshot_message_dispatch_type(g_message_dispatch_metrics.stream_response);
    snapshot.request_proceed = snapshot_message_dispatch_type(g_message_dispatch_metrics.request_proceed);
    snapshot.defer_processing = snapshot_message_dispatch_type(g_message_dispatch_metrics.defer_processing);
    snapshot.other = snapshot_message_dispatch_type(g_message_dispatch_metrics.other);
    return snapshot;
}

void record_message_dispatch_enqueue(std::string_view type) {
    auto& state = get_message_dispatch_metrics_state(type);
    state.queued.fetch_add(1, std::memory_order_relaxed);
    state.cumulative_messages.fetch_add(1, std::memory_order_relaxed);
}

void record_message_dispatch_dequeue(std::string_view type, uint64_t wait_ms) {
    auto& state = get_message_dispatch_metrics_state(type);
    state.queued.fetch_sub(1, std::memory_order_relaxed);
    state.last_queue_ms.store(wait_ms, std::memory_order_relaxed);

    auto prev_max = state.max_queue_ms.load(std::memory_order_relaxed);
    while (wait_ms > prev_max &&
           !state.max_queue_ms.compare_exchange_weak(prev_max, wait_ms, std::memory_order_relaxed)) {
    }
}

response_flow_metrics_snapshot_t get_response_flow_metrics_snapshot() {
    return snapshot_response_flow_metrics();
}

void record_response_defer_schedule(uint64_t timeout_ms) {
    g_response_flow_metrics.active_deferred_requests.fetch_add(1, std::memory_order_relaxed);
    g_response_flow_metrics.cumulative_defer_schedules.fetch_add(1, std::memory_order_relaxed);
    g_response_flow_metrics.last_defer_timeout_ms.store(timeout_ms, std::memory_order_relaxed);
}

void record_response_defer_callback(uint64_t actual_delay_ms, uint64_t defer_count_for_request) {
    g_response_flow_metrics.active_deferred_requests.fetch_sub(1, std::memory_order_relaxed);
    g_response_flow_metrics.cumulative_defer_callbacks.fetch_add(1, std::memory_order_relaxed);
    g_response_flow_metrics.last_defer_actual_ms.store(actual_delay_ms, std::memory_order_relaxed);
    g_response_flow_metrics.last_defer_count_per_request.store(defer_count_for_request, std::memory_order_relaxed);
    auto prev_max = g_response_flow_metrics.max_defer_actual_ms.load(std::memory_order_relaxed);
    while (actual_delay_ms > prev_max &&
           !g_response_flow_metrics.max_defer_actual_ms.compare_exchange_weak(prev_max, actual_delay_ms,
                                                                              std::memory_order_relaxed)) {
    }
}

void record_response_proceed() {
    g_response_flow_metrics.cumulative_response_proceeds.fetch_add(1, std::memory_order_relaxed);
}

void record_response_send(bool final_send, uint64_t send_calls_for_request, uint64_t proceed_count_for_request,
                          uint64_t defer_count_for_request, uint64_t first_send_delay_ms, uint64_t send_window_ms) {
    g_response_flow_metrics.cumulative_response_send_calls.fetch_add(1, std::memory_order_relaxed);
    if(final_send) {
        g_response_flow_metrics.cumulative_response_final_sends.fetch_add(1, std::memory_order_relaxed);
    }

    g_response_flow_metrics.last_send_calls_per_request.store(send_calls_for_request, std::memory_order_relaxed);
    g_response_flow_metrics.last_proceed_count_per_request.store(proceed_count_for_request, std::memory_order_relaxed);
    g_response_flow_metrics.last_defer_count_per_request.store(defer_count_for_request, std::memory_order_relaxed);
    g_response_flow_metrics.last_first_send_delay_ms.store(first_send_delay_ms, std::memory_order_relaxed);
    g_response_flow_metrics.last_send_window_ms.store(send_window_ms, std::memory_order_relaxed);
    g_response_flow_metrics.last_final_sent.store(final_send, std::memory_order_relaxed);
}

void http_req::record_lifecycle_metrics(const http_req& req, const std::string& route, uint64_t total_ms) {
    const auto auth_ms = req.auth_duration_us.load(std::memory_order_relaxed) / 1000;
    const auto handler_dispatch_ts = req.handler_dispatch_ts_us.load(std::memory_order_relaxed);
    const auto handler_start_ts = req.handler_start_ts_us.load(std::memory_order_relaxed);
    const auto handler_end_ts = req.handler_end_ts_us.load(std::memory_order_relaxed);
    const auto response_dispatch_ts = req.response_dispatch_ts_us.load(std::memory_order_relaxed);
    const auto response_start_ts = req.response_start_ts_us.load(std::memory_order_relaxed);
    const auto response_progress_ts = req.response_progress_ts_us.load(std::memory_order_relaxed);
    const auto response_first_send_ts = req.response_first_send_ts_us.load(std::memory_order_relaxed);
    const auto response_last_send_ts = req.response_last_send_ts_us.load(std::memory_order_relaxed);
    const auto response_send_calls = req.response_send_count.load(std::memory_order_relaxed);
    const auto response_proceed_count = req.response_proceed_count.load(std::memory_order_relaxed);
    const auto response_defer_count = req.response_defer_count.load(std::memory_order_relaxed);
    const auto response_final_sent = req.response_final_sent.load(std::memory_order_relaxed);

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

    uint64_t response_dispatch_ms = 0;
    if (handler_end_ts != 0 && response_dispatch_ts >= handler_end_ts) {
        response_dispatch_ms = (response_dispatch_ts - handler_end_ts) / 1000;
    }

    uint64_t response_queue_ms = 0;
    if (response_dispatch_ts != 0 && response_start_ts >= response_dispatch_ts) {
        response_queue_ms = (response_start_ts - response_dispatch_ts) / 1000;
    }

    uint64_t response_progress_ms = 0;
    if (response_start_ts != 0 && response_progress_ts >= response_start_ts) {
        response_progress_ms = (response_progress_ts - response_start_ts) / 1000;
    }

    uint64_t response_first_send_delay_ms = 0;
    if (response_start_ts != 0 && response_first_send_ts >= response_start_ts) {
        response_first_send_delay_ms = (response_first_send_ts - response_start_ts) / 1000;
    }

    uint64_t response_send_window_ms = 0;
    if (response_first_send_ts != 0 && response_last_send_ts >= response_first_send_ts) {
        response_send_window_ms = (response_last_send_ts - response_first_send_ts) / 1000;
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
    g_http_request_metrics.last_response_dispatch_ms.store(response_dispatch_ms, std::memory_order_relaxed);
    g_http_request_metrics.last_response_queue_ms.store(response_queue_ms, std::memory_order_relaxed);
    g_http_request_metrics.last_response_progress_ms.store(response_progress_ms, std::memory_order_relaxed);
    g_http_request_metrics.last_response_send_calls.store(response_send_calls, std::memory_order_relaxed);
    g_http_request_metrics.last_response_proceed_count.store(response_proceed_count, std::memory_order_relaxed);
    g_http_request_metrics.last_response_defer_count.store(response_defer_count, std::memory_order_relaxed);
    g_http_request_metrics.last_response_first_send_delay_ms.store(response_first_send_delay_ms, std::memory_order_relaxed);
    g_http_request_metrics.last_response_send_window_ms.store(response_send_window_ms, std::memory_order_relaxed);
    g_http_request_metrics.last_response_final_sent.store(response_final_sent, std::memory_order_relaxed);
    g_http_request_metrics.last_is_write.store(req.is_write.load(std::memory_order_relaxed), std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_http_request_metrics.last_route_mutex);
        g_http_request_metrics.last_route = route;
    }

    if (route.find("POST /collections/") == 0 && route.find("/documents/import") != std::string::npos) {
        g_http_request_metrics.import_last_total_ms.store(total_ms, std::memory_order_relaxed);
        g_http_request_metrics.import_last_auth_ms.store(auth_ms, std::memory_order_relaxed);
        g_http_request_metrics.import_last_handler_wait_ms.store(handler_wait_ms, std::memory_order_relaxed);
        g_http_request_metrics.import_last_handler_ms.store(handler_ms, std::memory_order_relaxed);
        g_http_request_metrics.import_last_unattributed_ms.store(unattributed_ms, std::memory_order_relaxed);
        g_http_request_metrics.import_last_conn_to_start_ms.store(conn_to_start_ms, std::memory_order_relaxed);
        g_http_request_metrics.import_last_response_dispatch_ms.store(response_dispatch_ms, std::memory_order_relaxed);
        g_http_request_metrics.import_last_response_queue_ms.store(response_queue_ms, std::memory_order_relaxed);
        g_http_request_metrics.import_last_response_progress_ms.store(response_progress_ms, std::memory_order_relaxed);
        g_http_request_metrics.import_last_response_send_calls.store(response_send_calls, std::memory_order_relaxed);
        g_http_request_metrics.import_last_response_proceed_count.store(response_proceed_count, std::memory_order_relaxed);
        g_http_request_metrics.import_last_response_defer_count.store(response_defer_count, std::memory_order_relaxed);
        g_http_request_metrics.import_last_response_first_send_delay_ms.store(response_first_send_delay_ms,
                                                                              std::memory_order_relaxed);
        g_http_request_metrics.import_last_response_send_window_ms.store(response_send_window_ms,
                                                                         std::memory_order_relaxed);
        g_http_request_metrics.import_last_response_final_sent.store(response_final_sent, std::memory_order_relaxed);
    }
}
