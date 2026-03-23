#include "http_data.h"

#include <atomic>
#include <mutex>
#include <sstream>

#include "collection.h"

namespace {

struct http_request_metrics_state_t {
    std::atomic<uint64_t> cumulative_requests{0};
    std::atomic<uint64_t> cumulative_slow_requests{0};
    std::atomic<uint64_t> last_total_ms{0};
    std::atomic<uint64_t> last_auth_ms{0};
    std::atomic<uint64_t> last_handler_wait_ms{0};
    std::atomic<uint64_t> last_handler_ms{0};
    std::atomic<uint64_t> last_unattributed_ms{0};
    std::atomic<uint64_t> last_request_entry_ms{0};
    std::atomic<uint64_t> last_conn_to_start_ms{0};
    std::atomic<uint64_t> last_response_dispatch_ms{0};
    std::atomic<uint64_t> last_response_pre_dispatch_wait_ms{0};
    std::atomic<uint64_t> last_response_queue_ms{0};
    std::atomic<uint64_t> last_response_progress_ms{0};
    std::atomic<uint64_t> last_response_send_calls{0};
    std::atomic<uint64_t> last_response_proceed_count{0};
    std::atomic<uint64_t> last_response_defer_count{0};
    std::atomic<uint64_t> last_response_first_send_delay_ms{0};
    std::atomic<uint64_t> last_response_send_window_ms{0};
    std::atomic<uint64_t> last_h2o_header_ms{0};
    std::atomic<uint64_t> last_h2o_body_ms{0};
    std::atomic<uint64_t> last_h2o_request_total_ms{0};
    std::atomic<uint64_t> last_h2o_process_ms{0};
    std::atomic<uint64_t> last_h2o_response_ms{0};
    std::atomic<uint64_t> last_h2o_total_ms{0};
    std::atomic<bool> last_response_final_sent{false};
    std::atomic<bool> last_is_write{false};
    std::mutex last_route_mutex;
    std::string last_route;

    std::atomic<uint64_t> import_last_total_ms{0};
    std::atomic<uint64_t> import_last_auth_ms{0};
    std::atomic<uint64_t> import_last_handler_wait_ms{0};
    std::atomic<uint64_t> import_last_handler_ms{0};
    std::atomic<uint64_t> import_last_unattributed_ms{0};
    std::atomic<uint64_t> import_last_request_entry_ms{0};
    std::atomic<uint64_t> import_last_conn_to_start_ms{0};
    std::atomic<uint64_t> import_last_response_dispatch_ms{0};
    std::atomic<uint64_t> import_last_response_pre_dispatch_wait_ms{0};
    std::atomic<uint64_t> import_last_response_queue_ms{0};
    std::atomic<uint64_t> import_last_response_progress_ms{0};
    std::atomic<uint64_t> import_last_response_send_calls{0};
    std::atomic<uint64_t> import_last_response_proceed_count{0};
    std::atomic<uint64_t> import_last_response_defer_count{0};
    std::atomic<uint64_t> import_last_response_first_send_delay_ms{0};
    std::atomic<uint64_t> import_last_response_send_window_ms{0};
    std::atomic<uint64_t> import_last_h2o_header_ms{0};
    std::atomic<uint64_t> import_last_h2o_body_ms{0};
    std::atomic<uint64_t> import_last_h2o_request_total_ms{0};
    std::atomic<uint64_t> import_last_h2o_process_ms{0};
    std::atomic<uint64_t> import_last_h2o_response_ms{0};
    std::atomic<uint64_t> import_last_h2o_total_ms{0};
    std::atomic<bool> import_last_response_final_sent{false};
    std::atomic<uint64_t> import_cumulative_requests{0};
    std::atomic<uint64_t> import_cumulative_total_ms{0};
    std::atomic<uint64_t> import_cumulative_auth_ms{0};
    std::atomic<uint64_t> import_cumulative_handler_wait_ms{0};
    std::atomic<uint64_t> import_cumulative_handler_ms{0};
    std::atomic<uint64_t> import_cumulative_unattributed_ms{0};
    std::atomic<uint64_t> import_cumulative_request_entry_ms{0};
    std::atomic<uint64_t> import_cumulative_response_pre_dispatch_wait_ms{0};
    std::atomic<uint64_t> import_cumulative_response_queue_ms{0};
    std::atomic<uint64_t> import_cumulative_h2o_request_total_ms{0};
    std::atomic<uint64_t> import_cumulative_h2o_process_ms{0};
    std::atomic<uint64_t> import_cumulative_h2o_response_ms{0};
    std::atomic<uint64_t> import_cumulative_h2o_total_ms{0};
    std::atomic<uint64_t> import_max_total_ms{0};
};

http_request_metrics_state_t g_http_request_metrics;

struct http_route_lifecycle_metrics_state_t {
    std::atomic<uint64_t> cumulative_requests{0};
    std::atomic<uint64_t> last_total_ms{0};
    std::atomic<uint64_t> last_auth_ms{0};
    std::atomic<uint64_t> last_handler_wait_ms{0};
    std::atomic<uint64_t> last_handler_ms{0};
    std::atomic<uint64_t> last_unattributed_ms{0};
    std::atomic<uint64_t> last_request_entry_ms{0};
    std::atomic<uint64_t> last_conn_to_start_ms{0};
    std::atomic<uint64_t> last_response_dispatch_ms{0};
    std::atomic<uint64_t> last_response_pre_dispatch_wait_ms{0};
    std::atomic<uint64_t> last_response_queue_ms{0};
    std::atomic<uint64_t> last_response_progress_ms{0};
    std::atomic<uint64_t> last_h2o_request_total_ms{0};
    std::atomic<uint64_t> last_h2o_total_ms{0};
    std::atomic<uint64_t> cumulative_total_ms{0};
    std::atomic<uint64_t> cumulative_auth_ms{0};
    std::atomic<uint64_t> cumulative_handler_wait_ms{0};
    std::atomic<uint64_t> cumulative_handler_ms{0};
    std::atomic<uint64_t> cumulative_unattributed_ms{0};
    std::atomic<uint64_t> cumulative_request_entry_ms{0};
    std::atomic<uint64_t> cumulative_conn_to_start_ms{0};
    std::atomic<uint64_t> cumulative_response_queue_ms{0};
    std::atomic<uint64_t> cumulative_response_pre_dispatch_wait_ms{0};
    std::atomic<uint64_t> cumulative_h2o_request_total_ms{0};
    std::atomic<uint64_t> cumulative_h2o_total_ms{0};
    std::atomic<uint64_t> max_total_ms{0};
};

struct hot_http_route_metrics_state_t {
    http_route_lifecycle_metrics_state_t health;
    http_route_lifecycle_metrics_state_t collections;
    http_route_lifecycle_metrics_state_t stats_json;
    http_route_lifecycle_metrics_state_t metrics_json;
    http_route_lifecycle_metrics_state_t search;
};

hot_http_route_metrics_state_t g_hot_http_route_metrics;

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

uint64_t average_or_zero(uint64_t total, uint64_t count) {
    return count == 0 ? 0 : (total / count);
}

bool timeval_is_nonzero(const timeval& value) {
    return value.tv_sec != 0 || value.tv_usec != 0;
}

uint64_t timeval_to_us(const timeval& value) {
    return (static_cast<uint64_t>(value.tv_sec) * 1000 * 1000) + static_cast<uint64_t>(value.tv_usec);
}

uint64_t duration_ms_between(const timeval& from, const timeval& until) {
    if(!timeval_is_nonzero(from) || !timeval_is_nonzero(until)) {
        return 0;
    }

    const uint64_t from_us = timeval_to_us(from);
    const uint64_t until_us = timeval_to_us(until);
    return until_us >= from_us ? (until_us - from_us) / 1000 : 0;
}

struct request_lifecycle_breakdown_t {
    uint64_t auth_ms = 0;
    uint64_t handler_wait_ms = 0;
    uint64_t handler_ms = 0;
    uint64_t unattributed_ms = 0;
    uint64_t request_entry_ms = 0;
    uint64_t conn_to_start_ms = 0;
    uint64_t response_dispatch_ms = 0;
    uint64_t response_pre_dispatch_wait_ms = 0;
    uint64_t response_queue_ms = 0;
    uint64_t response_progress_ms = 0;
    uint64_t response_first_send_delay_ms = 0;
    uint64_t response_send_window_ms = 0;
    uint64_t response_send_calls = 0;
    uint64_t response_proceed_count = 0;
    uint64_t response_defer_count = 0;
    uint64_t h2o_header_ms = 0;
    uint64_t h2o_body_ms = 0;
    uint64_t h2o_request_total_ms = 0;
    uint64_t h2o_process_ms = 0;
    uint64_t h2o_response_ms = 0;
    uint64_t h2o_total_ms = 0;
    bool response_final_sent = false;
};

request_lifecycle_breakdown_t compute_request_lifecycle_breakdown(const http_req& req, uint64_t total_ms) {
    request_lifecycle_breakdown_t breakdown;
    breakdown.auth_ms = req.auth_duration_us.load(std::memory_order_relaxed) / 1000;

    const auto handler_dispatch_ts = req.handler_dispatch_ts_us.load(std::memory_order_relaxed);
    const auto handler_start_ts = req.handler_start_ts_us.load(std::memory_order_relaxed);
    const auto handler_end_ts = req.handler_end_ts_us.load(std::memory_order_relaxed);
    const auto response_dispatch_ts = req.response_dispatch_ts_us.load(std::memory_order_relaxed);
    const auto response_pre_dispatch_wait_us = req.response_pre_dispatch_wait_us.load(std::memory_order_relaxed);
    const auto response_start_ts = req.response_start_ts_us.load(std::memory_order_relaxed);
    const auto response_progress_ts = req.response_progress_ts_us.load(std::memory_order_relaxed);
    const auto response_first_send_ts = req.response_first_send_ts_us.load(std::memory_order_relaxed);
    const auto response_last_send_ts = req.response_last_send_ts_us.load(std::memory_order_relaxed);
    breakdown.response_send_calls = req.response_send_count.load(std::memory_order_relaxed);
    breakdown.response_proceed_count = req.response_proceed_count.load(std::memory_order_relaxed);
    breakdown.response_defer_count = req.response_defer_count.load(std::memory_order_relaxed);
    breakdown.response_final_sent = req.response_final_sent.load(std::memory_order_relaxed);

    if (handler_dispatch_ts != 0 && handler_start_ts >= handler_dispatch_ts) {
        breakdown.handler_wait_ms = (handler_start_ts - handler_dispatch_ts) / 1000;
    }
    if (handler_start_ts != 0 && handler_end_ts >= handler_start_ts) {
        breakdown.handler_ms = (handler_end_ts - handler_start_ts) / 1000;
    }
    if (req.start_ts >= req.conn_ts) {
        breakdown.conn_to_start_ms = (req.start_ts - req.conn_ts) / 1000;
    }
    if (handler_end_ts != 0 && response_dispatch_ts >= handler_end_ts) {
        breakdown.response_dispatch_ms = (response_dispatch_ts - handler_end_ts) / 1000;
    }
    breakdown.response_pre_dispatch_wait_ms = response_pre_dispatch_wait_us / 1000;
    if (response_dispatch_ts != 0 && response_start_ts >= response_dispatch_ts) {
        breakdown.response_queue_ms = (response_start_ts - response_dispatch_ts) / 1000;
    }
    if (response_start_ts != 0 && response_progress_ts >= response_start_ts) {
        breakdown.response_progress_ms = (response_progress_ts - response_start_ts) / 1000;
    }
    if (response_start_ts != 0 && response_first_send_ts >= response_start_ts) {
        breakdown.response_first_send_delay_ms = (response_first_send_ts - response_start_ts) / 1000;
    }
    if (response_first_send_ts != 0 && response_last_send_ts >= response_first_send_ts) {
        breakdown.response_send_window_ms = (response_last_send_ts - response_first_send_ts) / 1000;
    }

    if(req._req != nullptr) {
        const auto& request_begin_at = req._req->timestamps.request_begin_at;
        const auto& request_body_begin_at = req._req->timestamps.request_body_begin_at;
        const auto& processed_at = req._req->processed_at.at;
        const auto& response_start_at = req._req->timestamps.response_start_at;
        const auto& response_end_at = req._req->timestamps.response_end_at;
        if(timeval_is_nonzero(request_begin_at)) {
            const auto request_begin_us = timeval_to_us(request_begin_at);
            if(req.request_entry_ts_us >= request_begin_us) {
                breakdown.request_entry_ms = (req.request_entry_ts_us - request_begin_us) / 1000;
            }
        }
        const timeval header_until = timeval_is_nonzero(request_body_begin_at) ? request_body_begin_at : processed_at;
        const timeval body_from = timeval_is_nonzero(request_body_begin_at) ? request_body_begin_at : processed_at;
        breakdown.h2o_header_ms = duration_ms_between(request_begin_at, header_until);
        breakdown.h2o_body_ms = duration_ms_between(body_from, processed_at);
        breakdown.h2o_request_total_ms = duration_ms_between(request_begin_at, processed_at);
        breakdown.h2o_process_ms = duration_ms_between(processed_at, response_start_at);
        breakdown.h2o_response_ms = duration_ms_between(response_start_at, response_end_at);
        breakdown.h2o_total_ms = duration_ms_between(request_begin_at, response_end_at);
    }

    const uint64_t attributed_ms = breakdown.auth_ms + breakdown.handler_wait_ms + breakdown.handler_ms;
    breakdown.unattributed_ms = total_ms >= attributed_ms ? (total_ms - attributed_ms) : 0;
    return breakdown;
}

http_route_lifecycle_metrics_snapshot_t snapshot_hot_http_route_metrics(
    const http_route_lifecycle_metrics_state_t& state) {
    http_route_lifecycle_metrics_snapshot_t snapshot;
    snapshot.cumulative_requests = state.cumulative_requests.load(std::memory_order_relaxed);
    snapshot.last_total_ms = state.last_total_ms.load(std::memory_order_relaxed);
    snapshot.last_auth_ms = state.last_auth_ms.load(std::memory_order_relaxed);
    snapshot.last_handler_wait_ms = state.last_handler_wait_ms.load(std::memory_order_relaxed);
    snapshot.last_handler_ms = state.last_handler_ms.load(std::memory_order_relaxed);
    snapshot.last_unattributed_ms = state.last_unattributed_ms.load(std::memory_order_relaxed);
    snapshot.last_request_entry_ms = state.last_request_entry_ms.load(std::memory_order_relaxed);
    snapshot.last_conn_to_start_ms = state.last_conn_to_start_ms.load(std::memory_order_relaxed);
    snapshot.last_response_dispatch_ms = state.last_response_dispatch_ms.load(std::memory_order_relaxed);
    snapshot.last_response_pre_dispatch_wait_ms =
        state.last_response_pre_dispatch_wait_ms.load(std::memory_order_relaxed);
    snapshot.last_response_queue_ms = state.last_response_queue_ms.load(std::memory_order_relaxed);
    snapshot.last_response_progress_ms = state.last_response_progress_ms.load(std::memory_order_relaxed);
    snapshot.last_h2o_request_total_ms = state.last_h2o_request_total_ms.load(std::memory_order_relaxed);
    snapshot.last_h2o_total_ms = state.last_h2o_total_ms.load(std::memory_order_relaxed);
    snapshot.max_total_ms = state.max_total_ms.load(std::memory_order_relaxed);
    snapshot.avg_total_ms = average_or_zero(
        state.cumulative_total_ms.load(std::memory_order_relaxed), snapshot.cumulative_requests);
    snapshot.avg_auth_ms = average_or_zero(
        state.cumulative_auth_ms.load(std::memory_order_relaxed), snapshot.cumulative_requests);
    snapshot.avg_handler_wait_ms = average_or_zero(
        state.cumulative_handler_wait_ms.load(std::memory_order_relaxed), snapshot.cumulative_requests);
    snapshot.avg_handler_ms = average_or_zero(
        state.cumulative_handler_ms.load(std::memory_order_relaxed), snapshot.cumulative_requests);
    snapshot.avg_unattributed_ms = average_or_zero(
        state.cumulative_unattributed_ms.load(std::memory_order_relaxed), snapshot.cumulative_requests);
    snapshot.avg_request_entry_ms = average_or_zero(
        state.cumulative_request_entry_ms.load(std::memory_order_relaxed), snapshot.cumulative_requests);
    snapshot.avg_conn_to_start_ms = average_or_zero(
        state.cumulative_conn_to_start_ms.load(std::memory_order_relaxed), snapshot.cumulative_requests);
    snapshot.avg_response_queue_ms = average_or_zero(
        state.cumulative_response_queue_ms.load(std::memory_order_relaxed), snapshot.cumulative_requests);
    snapshot.avg_response_pre_dispatch_wait_ms = average_or_zero(
        state.cumulative_response_pre_dispatch_wait_ms.load(std::memory_order_relaxed),
        snapshot.cumulative_requests);
    snapshot.avg_h2o_request_total_ms = average_or_zero(
        state.cumulative_h2o_request_total_ms.load(std::memory_order_relaxed), snapshot.cumulative_requests);
    snapshot.avg_h2o_total_ms = average_or_zero(
        state.cumulative_h2o_total_ms.load(std::memory_order_relaxed), snapshot.cumulative_requests);
    return snapshot;
}

http_route_lifecycle_metrics_state_t* get_hot_http_route_metrics_state(const http_req& req) {
    if(req.http_method == "GET") {
        if(req.path_without_query == "/health") {
            return &g_hot_http_route_metrics.health;
        }

        if(req.path_without_query == "/collections") {
            return &g_hot_http_route_metrics.collections;
        }

        if(req.path_without_query == "/stats.json") {
            return &g_hot_http_route_metrics.stats_json;
        }

        if(req.path_without_query == "/metrics.json") {
            return &g_hot_http_route_metrics.metrics_json;
        }
    }

    if(req.path_without_query == "/multi_search" ||
       StringUtils::ends_with(req.path_without_query, "/documents/search")) {
        return &g_hot_http_route_metrics.search;
    }

    return nullptr;
}

void record_hot_http_route_metrics(http_route_lifecycle_metrics_state_t& state, uint64_t total_ms, uint64_t auth_ms,
                                   uint64_t handler_wait_ms, uint64_t handler_ms, uint64_t unattributed_ms,
                                   uint64_t request_entry_ms, uint64_t conn_to_start_ms, uint64_t response_dispatch_ms,
                                   uint64_t response_pre_dispatch_wait_ms, uint64_t response_queue_ms,
                                   uint64_t response_progress_ms, uint64_t h2o_request_total_ms,
                                   uint64_t h2o_total_ms) {
    state.cumulative_requests.fetch_add(1, std::memory_order_relaxed);
    state.last_total_ms.store(total_ms, std::memory_order_relaxed);
    state.last_auth_ms.store(auth_ms, std::memory_order_relaxed);
    state.last_handler_wait_ms.store(handler_wait_ms, std::memory_order_relaxed);
    state.last_handler_ms.store(handler_ms, std::memory_order_relaxed);
    state.last_unattributed_ms.store(unattributed_ms, std::memory_order_relaxed);
    state.last_request_entry_ms.store(request_entry_ms, std::memory_order_relaxed);
    state.last_conn_to_start_ms.store(conn_to_start_ms, std::memory_order_relaxed);
    state.last_response_dispatch_ms.store(response_dispatch_ms, std::memory_order_relaxed);
    state.last_response_pre_dispatch_wait_ms.store(response_pre_dispatch_wait_ms, std::memory_order_relaxed);
    state.last_response_queue_ms.store(response_queue_ms, std::memory_order_relaxed);
    state.last_response_progress_ms.store(response_progress_ms, std::memory_order_relaxed);
    state.last_h2o_request_total_ms.store(h2o_request_total_ms, std::memory_order_relaxed);
    state.last_h2o_total_ms.store(h2o_total_ms, std::memory_order_relaxed);
    state.cumulative_total_ms.fetch_add(total_ms, std::memory_order_relaxed);
    state.cumulative_auth_ms.fetch_add(auth_ms, std::memory_order_relaxed);
    state.cumulative_handler_wait_ms.fetch_add(handler_wait_ms, std::memory_order_relaxed);
    state.cumulative_handler_ms.fetch_add(handler_ms, std::memory_order_relaxed);
    state.cumulative_unattributed_ms.fetch_add(unattributed_ms, std::memory_order_relaxed);
    state.cumulative_request_entry_ms.fetch_add(request_entry_ms, std::memory_order_relaxed);
    state.cumulative_conn_to_start_ms.fetch_add(conn_to_start_ms, std::memory_order_relaxed);
    state.cumulative_response_queue_ms.fetch_add(response_queue_ms, std::memory_order_relaxed);
    state.cumulative_response_pre_dispatch_wait_ms.fetch_add(response_pre_dispatch_wait_ms, std::memory_order_relaxed);
    state.cumulative_h2o_request_total_ms.fetch_add(h2o_request_total_ms, std::memory_order_relaxed);
    state.cumulative_h2o_total_ms.fetch_add(h2o_total_ms, std::memory_order_relaxed);

    auto prev_max = state.max_total_ms.load(std::memory_order_relaxed);
    while(total_ms > prev_max &&
          !state.max_total_ms.compare_exchange_weak(prev_max, total_ms, std::memory_order_relaxed)) {
    }
}

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

hot_http_route_metrics_snapshot_t get_hot_http_route_metrics_snapshot() {
    hot_http_route_metrics_snapshot_t snapshot;
    snapshot.health = snapshot_hot_http_route_metrics(g_hot_http_route_metrics.health);
    snapshot.collections = snapshot_hot_http_route_metrics(g_hot_http_route_metrics.collections);
    snapshot.stats_json = snapshot_hot_http_route_metrics(g_hot_http_route_metrics.stats_json);
    snapshot.metrics_json = snapshot_hot_http_route_metrics(g_hot_http_route_metrics.metrics_json);
    snapshot.search = snapshot_hot_http_route_metrics(g_hot_http_route_metrics.search);
    return snapshot;
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
    snapshot.last_request_entry_ms = g_http_request_metrics.last_request_entry_ms.load(std::memory_order_relaxed);
    snapshot.last_conn_to_start_ms = g_http_request_metrics.last_conn_to_start_ms.load(std::memory_order_relaxed);
    snapshot.last_response_dispatch_ms = g_http_request_metrics.last_response_dispatch_ms.load(std::memory_order_relaxed);
    snapshot.last_response_pre_dispatch_wait_ms =
        g_http_request_metrics.last_response_pre_dispatch_wait_ms.load(std::memory_order_relaxed);
    snapshot.last_response_queue_ms = g_http_request_metrics.last_response_queue_ms.load(std::memory_order_relaxed);
    snapshot.last_response_progress_ms = g_http_request_metrics.last_response_progress_ms.load(std::memory_order_relaxed);
    snapshot.last_response_send_calls = g_http_request_metrics.last_response_send_calls.load(std::memory_order_relaxed);
    snapshot.last_response_proceed_count = g_http_request_metrics.last_response_proceed_count.load(std::memory_order_relaxed);
    snapshot.last_response_defer_count = g_http_request_metrics.last_response_defer_count.load(std::memory_order_relaxed);
    snapshot.last_response_first_send_delay_ms = g_http_request_metrics.last_response_first_send_delay_ms.load(std::memory_order_relaxed);
    snapshot.last_response_send_window_ms = g_http_request_metrics.last_response_send_window_ms.load(std::memory_order_relaxed);
    snapshot.last_h2o_header_ms = g_http_request_metrics.last_h2o_header_ms.load(std::memory_order_relaxed);
    snapshot.last_h2o_body_ms = g_http_request_metrics.last_h2o_body_ms.load(std::memory_order_relaxed);
    snapshot.last_h2o_request_total_ms = g_http_request_metrics.last_h2o_request_total_ms.load(std::memory_order_relaxed);
    snapshot.last_h2o_process_ms = g_http_request_metrics.last_h2o_process_ms.load(std::memory_order_relaxed);
    snapshot.last_h2o_response_ms = g_http_request_metrics.last_h2o_response_ms.load(std::memory_order_relaxed);
    snapshot.last_h2o_total_ms = g_http_request_metrics.last_h2o_total_ms.load(std::memory_order_relaxed);
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
    snapshot.import_last_request_entry_ms =
        g_http_request_metrics.import_last_request_entry_ms.load(std::memory_order_relaxed);
    snapshot.import_last_conn_to_start_ms = g_http_request_metrics.import_last_conn_to_start_ms.load(std::memory_order_relaxed);
    snapshot.import_last_response_dispatch_ms = g_http_request_metrics.import_last_response_dispatch_ms.load(std::memory_order_relaxed);
    snapshot.import_last_response_pre_dispatch_wait_ms =
        g_http_request_metrics.import_last_response_pre_dispatch_wait_ms.load(std::memory_order_relaxed);
    snapshot.import_last_response_queue_ms = g_http_request_metrics.import_last_response_queue_ms.load(std::memory_order_relaxed);
    snapshot.import_last_response_progress_ms = g_http_request_metrics.import_last_response_progress_ms.load(std::memory_order_relaxed);
    snapshot.import_last_response_send_calls = g_http_request_metrics.import_last_response_send_calls.load(std::memory_order_relaxed);
    snapshot.import_last_response_proceed_count = g_http_request_metrics.import_last_response_proceed_count.load(std::memory_order_relaxed);
    snapshot.import_last_response_defer_count = g_http_request_metrics.import_last_response_defer_count.load(std::memory_order_relaxed);
    snapshot.import_last_response_first_send_delay_ms =
        g_http_request_metrics.import_last_response_first_send_delay_ms.load(std::memory_order_relaxed);
    snapshot.import_last_response_send_window_ms =
        g_http_request_metrics.import_last_response_send_window_ms.load(std::memory_order_relaxed);
    snapshot.import_last_h2o_header_ms = g_http_request_metrics.import_last_h2o_header_ms.load(std::memory_order_relaxed);
    snapshot.import_last_h2o_body_ms = g_http_request_metrics.import_last_h2o_body_ms.load(std::memory_order_relaxed);
    snapshot.import_last_h2o_request_total_ms =
        g_http_request_metrics.import_last_h2o_request_total_ms.load(std::memory_order_relaxed);
    snapshot.import_last_h2o_process_ms = g_http_request_metrics.import_last_h2o_process_ms.load(std::memory_order_relaxed);
    snapshot.import_last_h2o_response_ms =
        g_http_request_metrics.import_last_h2o_response_ms.load(std::memory_order_relaxed);
    snapshot.import_last_h2o_total_ms = g_http_request_metrics.import_last_h2o_total_ms.load(std::memory_order_relaxed);
    snapshot.import_last_response_final_sent =
        g_http_request_metrics.import_last_response_final_sent.load(std::memory_order_relaxed);
    snapshot.import_cumulative_requests =
        g_http_request_metrics.import_cumulative_requests.load(std::memory_order_relaxed);
    const uint64_t import_cumulative_requests = snapshot.import_cumulative_requests;
    snapshot.import_avg_total_ms = average_or_zero(
        g_http_request_metrics.import_cumulative_total_ms.load(std::memory_order_relaxed),
        import_cumulative_requests);
    snapshot.import_avg_auth_ms = average_or_zero(
        g_http_request_metrics.import_cumulative_auth_ms.load(std::memory_order_relaxed),
        import_cumulative_requests);
    snapshot.import_avg_handler_wait_ms = average_or_zero(
        g_http_request_metrics.import_cumulative_handler_wait_ms.load(std::memory_order_relaxed),
        import_cumulative_requests);
    snapshot.import_avg_handler_ms = average_or_zero(
        g_http_request_metrics.import_cumulative_handler_ms.load(std::memory_order_relaxed),
        import_cumulative_requests);
    snapshot.import_avg_unattributed_ms = average_or_zero(
        g_http_request_metrics.import_cumulative_unattributed_ms.load(std::memory_order_relaxed),
        import_cumulative_requests);
    snapshot.import_avg_request_entry_ms = average_or_zero(
        g_http_request_metrics.import_cumulative_request_entry_ms.load(std::memory_order_relaxed),
        import_cumulative_requests);
    snapshot.import_avg_response_queue_ms = average_or_zero(
        g_http_request_metrics.import_cumulative_response_queue_ms.load(std::memory_order_relaxed),
        import_cumulative_requests);
    snapshot.import_avg_response_pre_dispatch_wait_ms = average_or_zero(
        g_http_request_metrics.import_cumulative_response_pre_dispatch_wait_ms.load(std::memory_order_relaxed),
        import_cumulative_requests);
    snapshot.import_avg_h2o_request_total_ms = average_or_zero(
        g_http_request_metrics.import_cumulative_h2o_request_total_ms.load(std::memory_order_relaxed),
        import_cumulative_requests);
    snapshot.import_avg_h2o_process_ms = average_or_zero(
        g_http_request_metrics.import_cumulative_h2o_process_ms.load(std::memory_order_relaxed),
        import_cumulative_requests);
    snapshot.import_avg_h2o_response_ms = average_or_zero(
        g_http_request_metrics.import_cumulative_h2o_response_ms.load(std::memory_order_relaxed),
        import_cumulative_requests);
    snapshot.import_avg_h2o_total_ms = average_or_zero(
        g_http_request_metrics.import_cumulative_h2o_total_ms.load(std::memory_order_relaxed),
        import_cumulative_requests);
    snapshot.import_max_total_ms = g_http_request_metrics.import_max_total_ms.load(std::memory_order_relaxed);
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

std::string http_req::get_slow_request_log_suffix(uint64_t total_ms) const {
    const auto is_import_route = http_method == "POST" &&
                                 path_without_query.find("/collections/") == 0 &&
                                 path_without_query.find("/documents/import") != std::string::npos;
    if (!is_import_route) {
        return "";
    }

    const auto breakdown = compute_request_lifecycle_breakdown(*this, total_ms);
    const auto import_metrics = Collection::get_import_metrics_snapshot();
    std::ostringstream stream;
    stream << ", import_body_bytes=" << body.size()
           << ", import_conn_to_start_ms=" << breakdown.conn_to_start_ms
           << ", import_request_entry_ms=" << breakdown.request_entry_ms
           << ", import_auth_ms=" << breakdown.auth_ms
           << ", import_handler_wait_ms=" << breakdown.handler_wait_ms
           << ", import_handler_ms=" << breakdown.handler_ms
           << ", import_unattributed_ms=" << breakdown.unattributed_ms
           << ", import_response_dispatch_ms=" << breakdown.response_dispatch_ms
           << ", import_response_pre_dispatch_wait_ms=" << breakdown.response_pre_dispatch_wait_ms
           << ", import_response_queue_ms=" << breakdown.response_queue_ms
           << ", import_response_progress_ms=" << breakdown.response_progress_ms
           << ", import_response_send_calls=" << breakdown.response_send_calls
           << ", import_response_proceed_count=" << breakdown.response_proceed_count
           << ", import_response_defer_count=" << breakdown.response_defer_count
           << ", import_response_first_send_delay_ms=" << breakdown.response_first_send_delay_ms
           << ", import_response_send_window_ms=" << breakdown.response_send_window_ms
           << ", import_h2o_header_ms=" << breakdown.h2o_header_ms
           << ", import_h2o_body_ms=" << breakdown.h2o_body_ms
           << ", import_h2o_request_total_ms=" << breakdown.h2o_request_total_ms
           << ", import_h2o_process_ms=" << breakdown.h2o_process_ms
           << ", import_h2o_response_ms=" << breakdown.h2o_response_ms
           << ", import_h2o_total_ms=" << breakdown.h2o_total_ms
           << ", import_response_final_sent=" << (breakdown.response_final_sent ? 1 : 0)
           << ", helper_collection=" << import_metrics.last_collection_name
           << ", helper_field=" << import_metrics.last_async_reference_helper_field_name
           << ", helper_total_ms=" << import_metrics.last_async_reference_helper_total_ms
           << ", helper_matched_docs=" << import_metrics.last_async_reference_helper_matched_docs
           << ", helper_updated_docs=" << import_metrics.last_async_reference_helper_updated_docs
           << ", helper_chunks=" << import_metrics.last_async_reference_helper_chunks
           << ", helper_planned_chunk_docs=" << import_metrics.last_async_reference_helper_planned_chunk_docs
           << ", helper_max_chunk_docs=" << import_metrics.last_async_reference_helper_max_chunk_docs
           << ", helper_chunk_plan_sample_docs=" << import_metrics.last_async_reference_helper_chunk_plan_sample_docs
           << ", helper_chunk_target_bytes=" << import_metrics.last_async_reference_helper_chunk_target_bytes
           << ", helper_chunk_plan_estimated_total_doc_bytes="
           << import_metrics.last_async_reference_helper_chunk_plan_estimated_total_doc_bytes
           << ", helper_store_retry_writes=" << import_metrics.last_async_reference_helper_store_retry_writes
           << ", helper_write_failures=" << import_metrics.last_async_reference_helper_write_failures
           << ", helper_slow_paths=" << import_metrics.cumulative_async_reference_helper_slow_paths
           << ", helper_max_total_ms=" << import_metrics.max_async_reference_helper_total_ms;
    return stream.str();
}

void http_req::record_lifecycle_metrics(const http_req& req, const std::string& route, uint64_t total_ms) {
    const auto breakdown = compute_request_lifecycle_breakdown(req, total_ms);

    g_http_request_metrics.cumulative_requests.fetch_add(1, std::memory_order_relaxed);
    const auto slow_threshold_ms = Config::get_instance().get_log_slow_requests_time_ms();
    if (slow_threshold_ms >= 0 && total_ms >= static_cast<uint64_t>(slow_threshold_ms)) {
        g_http_request_metrics.cumulative_slow_requests.fetch_add(1, std::memory_order_relaxed);
    }
    g_http_request_metrics.last_total_ms.store(total_ms, std::memory_order_relaxed);
    g_http_request_metrics.last_auth_ms.store(breakdown.auth_ms, std::memory_order_relaxed);
    g_http_request_metrics.last_handler_wait_ms.store(breakdown.handler_wait_ms, std::memory_order_relaxed);
    g_http_request_metrics.last_handler_ms.store(breakdown.handler_ms, std::memory_order_relaxed);
    g_http_request_metrics.last_unattributed_ms.store(breakdown.unattributed_ms, std::memory_order_relaxed);
    g_http_request_metrics.last_request_entry_ms.store(breakdown.request_entry_ms, std::memory_order_relaxed);
    g_http_request_metrics.last_conn_to_start_ms.store(breakdown.conn_to_start_ms, std::memory_order_relaxed);
    g_http_request_metrics.last_response_dispatch_ms.store(breakdown.response_dispatch_ms, std::memory_order_relaxed);
    g_http_request_metrics.last_response_pre_dispatch_wait_ms.store(breakdown.response_pre_dispatch_wait_ms,
                                                                    std::memory_order_relaxed);
    g_http_request_metrics.last_response_queue_ms.store(breakdown.response_queue_ms, std::memory_order_relaxed);
    g_http_request_metrics.last_response_progress_ms.store(breakdown.response_progress_ms, std::memory_order_relaxed);
    g_http_request_metrics.last_response_send_calls.store(breakdown.response_send_calls, std::memory_order_relaxed);
    g_http_request_metrics.last_response_proceed_count.store(breakdown.response_proceed_count,
                                                             std::memory_order_relaxed);
    g_http_request_metrics.last_response_defer_count.store(breakdown.response_defer_count, std::memory_order_relaxed);
    g_http_request_metrics.last_response_first_send_delay_ms.store(breakdown.response_first_send_delay_ms,
                                                                   std::memory_order_relaxed);
    g_http_request_metrics.last_response_send_window_ms.store(breakdown.response_send_window_ms,
                                                              std::memory_order_relaxed);
    g_http_request_metrics.last_h2o_header_ms.store(breakdown.h2o_header_ms, std::memory_order_relaxed);
    g_http_request_metrics.last_h2o_body_ms.store(breakdown.h2o_body_ms, std::memory_order_relaxed);
    g_http_request_metrics.last_h2o_request_total_ms.store(breakdown.h2o_request_total_ms, std::memory_order_relaxed);
    g_http_request_metrics.last_h2o_process_ms.store(breakdown.h2o_process_ms, std::memory_order_relaxed);
    g_http_request_metrics.last_h2o_response_ms.store(breakdown.h2o_response_ms, std::memory_order_relaxed);
    g_http_request_metrics.last_h2o_total_ms.store(breakdown.h2o_total_ms, std::memory_order_relaxed);
    g_http_request_metrics.last_response_final_sent.store(breakdown.response_final_sent, std::memory_order_relaxed);
    g_http_request_metrics.last_is_write.store(req.is_write.load(std::memory_order_relaxed), std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_http_request_metrics.last_route_mutex);
        g_http_request_metrics.last_route = route;
    }

    if (route.find("POST /collections/") == 0 && route.find("/documents/import") != std::string::npos) {
        g_http_request_metrics.import_last_total_ms.store(total_ms, std::memory_order_relaxed);
        g_http_request_metrics.import_last_auth_ms.store(breakdown.auth_ms, std::memory_order_relaxed);
        g_http_request_metrics.import_last_handler_wait_ms.store(breakdown.handler_wait_ms, std::memory_order_relaxed);
        g_http_request_metrics.import_last_handler_ms.store(breakdown.handler_ms, std::memory_order_relaxed);
        g_http_request_metrics.import_last_unattributed_ms.store(breakdown.unattributed_ms, std::memory_order_relaxed);
        g_http_request_metrics.import_last_request_entry_ms.store(breakdown.request_entry_ms, std::memory_order_relaxed);
        g_http_request_metrics.import_last_conn_to_start_ms.store(breakdown.conn_to_start_ms, std::memory_order_relaxed);
        g_http_request_metrics.import_last_response_dispatch_ms.store(breakdown.response_dispatch_ms,
                                                                      std::memory_order_relaxed);
        g_http_request_metrics.import_last_response_pre_dispatch_wait_ms.store(breakdown.response_pre_dispatch_wait_ms,
                                                                               std::memory_order_relaxed);
        g_http_request_metrics.import_last_response_queue_ms.store(breakdown.response_queue_ms,
                                                                   std::memory_order_relaxed);
        g_http_request_metrics.import_last_response_progress_ms.store(breakdown.response_progress_ms,
                                                                      std::memory_order_relaxed);
        g_http_request_metrics.import_last_response_send_calls.store(breakdown.response_send_calls,
                                                                     std::memory_order_relaxed);
        g_http_request_metrics.import_last_response_proceed_count.store(breakdown.response_proceed_count,
                                                                        std::memory_order_relaxed);
        g_http_request_metrics.import_last_response_defer_count.store(breakdown.response_defer_count,
                                                                      std::memory_order_relaxed);
        g_http_request_metrics.import_last_response_first_send_delay_ms.store(breakdown.response_first_send_delay_ms,
                                                                              std::memory_order_relaxed);
        g_http_request_metrics.import_last_response_send_window_ms.store(breakdown.response_send_window_ms,
                                                                         std::memory_order_relaxed);
        g_http_request_metrics.import_last_h2o_header_ms.store(breakdown.h2o_header_ms, std::memory_order_relaxed);
        g_http_request_metrics.import_last_h2o_body_ms.store(breakdown.h2o_body_ms, std::memory_order_relaxed);
        g_http_request_metrics.import_last_h2o_request_total_ms.store(breakdown.h2o_request_total_ms,
                                                                      std::memory_order_relaxed);
        g_http_request_metrics.import_last_h2o_process_ms.store(breakdown.h2o_process_ms, std::memory_order_relaxed);
        g_http_request_metrics.import_last_h2o_response_ms.store(breakdown.h2o_response_ms, std::memory_order_relaxed);
        g_http_request_metrics.import_last_h2o_total_ms.store(breakdown.h2o_total_ms, std::memory_order_relaxed);
        g_http_request_metrics.import_last_response_final_sent.store(breakdown.response_final_sent,
                                                                     std::memory_order_relaxed);
        g_http_request_metrics.import_cumulative_requests.fetch_add(1, std::memory_order_relaxed);
        g_http_request_metrics.import_cumulative_total_ms.fetch_add(total_ms, std::memory_order_relaxed);
        g_http_request_metrics.import_cumulative_auth_ms.fetch_add(breakdown.auth_ms, std::memory_order_relaxed);
        g_http_request_metrics.import_cumulative_handler_wait_ms.fetch_add(breakdown.handler_wait_ms,
                                                                           std::memory_order_relaxed);
        g_http_request_metrics.import_cumulative_handler_ms.fetch_add(breakdown.handler_ms, std::memory_order_relaxed);
        g_http_request_metrics.import_cumulative_unattributed_ms.fetch_add(breakdown.unattributed_ms,
                                                                           std::memory_order_relaxed);
        g_http_request_metrics.import_cumulative_request_entry_ms.fetch_add(breakdown.request_entry_ms,
                                                                            std::memory_order_relaxed);
        g_http_request_metrics.import_cumulative_response_pre_dispatch_wait_ms.fetch_add(
            breakdown.response_pre_dispatch_wait_ms,
                                                                                         std::memory_order_relaxed);
        g_http_request_metrics.import_cumulative_response_queue_ms.fetch_add(breakdown.response_queue_ms,
                                                                             std::memory_order_relaxed);
        g_http_request_metrics.import_cumulative_h2o_request_total_ms.fetch_add(breakdown.h2o_request_total_ms,
                                                                                std::memory_order_relaxed);
        g_http_request_metrics.import_cumulative_h2o_process_ms.fetch_add(breakdown.h2o_process_ms,
                                                                          std::memory_order_relaxed);
        g_http_request_metrics.import_cumulative_h2o_response_ms.fetch_add(breakdown.h2o_response_ms,
                                                                           std::memory_order_relaxed);
        g_http_request_metrics.import_cumulative_h2o_total_ms.fetch_add(breakdown.h2o_total_ms,
                                                                        std::memory_order_relaxed);
        const uint64_t prev_max = g_http_request_metrics.import_max_total_ms.load(std::memory_order_relaxed);
        if (total_ms > prev_max) {
            g_http_request_metrics.import_max_total_ms.store(total_ms, std::memory_order_relaxed);
        }
    }

    auto* hot_route_metrics = get_hot_http_route_metrics_state(req);
    if(hot_route_metrics != nullptr) {
        record_hot_http_route_metrics(*hot_route_metrics, total_ms, breakdown.auth_ms, breakdown.handler_wait_ms,
                                      breakdown.handler_ms, breakdown.unattributed_ms, breakdown.request_entry_ms,
                                      breakdown.conn_to_start_ms, breakdown.response_dispatch_ms,
                                      breakdown.response_pre_dispatch_wait_ms, breakdown.response_queue_ms,
                                      breakdown.response_progress_ms, breakdown.h2o_request_total_ms,
                                      breakdown.h2o_total_ms);
    }
}
