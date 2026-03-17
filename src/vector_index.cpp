#include "vector_index.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <shared_mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include <usearch/index.hpp>
#include <usearch/index_plugins.hpp>

namespace {

namespace usearch = unum::usearch;

const float* prepare_query(const vector_index_t& index, const std::vector<float>& query,
                           std::vector<float>& normalized_query) {
    if (index.distance_type() != cosine) {
        return query.data();
    }

    normalized_query.resize(query.size());
    vector_index_t::normalize_vector(query, normalized_query);
    return normalized_query.data();
}

std::vector<float> prepare_vector_for_storage(const vector_index_t& index, const std::vector<float>& values) {
    if (index.distance_type() != cosine) {
        return values;
    }

    std::vector<float> normalized_values(values.size());
    vector_index_t::normalize_vector(values, normalized_values);
    return normalized_values;
}

size_t default_usearch_thread_count() {
    const auto hardware_threads = std::thread::hardware_concurrency();
    const auto desired_threads = hardware_threads == 0 ? size_t{16} : static_cast<size_t>(hardware_threads) * 2;
    return std::max<size_t>(64, desired_threads);
}

class usearch_vector_index_t final: public vector_index_t {
private:
    using key_t = std::uint32_t;
    using slot_t = std::uint32_t;
    using index_t = usearch::index_gt<float, key_t, slot_t>;

    struct metric_proxy_t {
        std::vector<std::vector<float>> const* slot_vectors = nullptr;
        usearch::metric_punned_t const* distance_metric = nullptr;

        template <typename member_at>
        static std::size_t slot_for(member_at const& member) noexcept {
            using usearch::get_slot;
            return static_cast<std::size_t>(get_slot(member));
        }

        float distance(float const* lhs, float const* rhs) const noexcept {
            return (*distance_metric)(reinterpret_cast<usearch::byte_t const*>(lhs),
                                      reinterpret_cast<usearch::byte_t const*>(rhs));
        }

        template <typename member_at>
        float operator()(float const* query, member_at const& member) const noexcept {
            auto const slot = slot_for(member);
            auto const& stored = (*slot_vectors)[slot];
            return distance(query, stored.data());
        }

        template <typename left_at, typename right_at,
                  typename = std::enable_if_t<!std::is_pointer_v<std::decay_t<left_at>> &&
                                              !std::is_pointer_v<std::decay_t<right_at>>>>
        float operator()(left_at const& left, right_at const& right) const noexcept {
            auto const left_slot = slot_for(left);
            auto const right_slot = slot_for(right);
            auto const& lhs = (*slot_vectors)[left_slot];
            auto const& rhs = (*slot_vectors)[right_slot];
            return distance(lhs.data(), rhs.data());
        }
    };

public:
    explicit usearch_vector_index_t(const vector_index_config_t& config):
        vector_index_t(config.num_dim, config.distance_type),
        config_(config),
        thread_count_(default_usearch_thread_count()),
        distance_metric_(config.num_dim, usearch::metric_kind_t::ip_k, usearch::scalar_kind_t::f32_k) {

        if (!distance_metric_) {
            throw std::runtime_error("Unable to configure USearch distance metric.");
        }

        auto index_result = index_t::make(usearch::index_config_t(config.M));
        if (!index_result) {
            throw std::runtime_error(index_result.error.release());
        }

        index_ = std::move(index_result.index);
        capacity_limit_ = std::max<size_t>(config.init_size, 1);
        if (!index_.reserve(usearch::index_limits_t(capacity_limit_, thread_count_))) {
            throw std::runtime_error("Unable to reserve USearch vector index capacity.");
        }

        slot_vectors_.reserve(capacity_limit_);
        tombstones_.reserve(capacity_limit_);
    }

    void ensure_capacity(size_t desired_total_elements) override {
        std::unique_lock lock(state_mutex_);
        ensure_capacity_locked(desired_total_elements);
    }

    void add_vector(uint32_t seq_id, const std::vector<float>& values) override {
        if (values.size() != num_dim()) {
            throw std::invalid_argument("Vector size mismatch.");
        }

        auto stored_values = prepare_vector_for_storage(*this, values);
        std::unique_lock lock(state_mutex_);
        auto const thread_slot = current_thread_slot();
        auto const update_config = make_update_config(thread_slot);
        metric_proxy_t metric{&slot_vectors_, &distance_metric_};

        auto existing_it = live_slots_.find(seq_id);
        if (existing_it != live_slots_.end()) {
            auto const slot = existing_it->second;
            slot_vectors_[slot] = std::move(stored_values);
            auto result = index_.update(index_.iterator_at(slot), seq_id, slot_vectors_[slot].data(), metric, update_config);
            if (!result) {
                throw std::runtime_error(result.error.release());
            }
            tombstones_[slot] = false;
            return;
        }

        if (!free_slots_.empty()) {
            auto const slot = free_slots_.back();
            free_slots_.pop_back();
            tombstones_[slot] = false;
            slot_vectors_[slot] = std::move(stored_values);
            live_slots_[seq_id] = slot;

            auto result = index_.update(index_.iterator_at(slot), seq_id, slot_vectors_[slot].data(), metric, update_config);
            if (!result) {
                live_slots_.erase(seq_id);
                tombstones_[slot] = true;
                free_slots_.push_back(slot);
                throw std::runtime_error(result.error.release());
            }
            return;
        }

        if (index_.size() >= capacity_limit_) {
            ensure_capacity_locked(index_.size() + 1);
        }

        slot_vectors_.push_back(std::move(stored_values));
        tombstones_.push_back(false);
        auto result = index_.add(seq_id, slot_vectors_.back().data(), metric, update_config);
        if (!result) {
            slot_vectors_.pop_back();
            tombstones_.pop_back();
            throw std::runtime_error(result.error.release());
        }

        auto const expected_slot = static_cast<slot_t>(slot_vectors_.size() - 1);
        if (result.slot != expected_slot) {
            throw std::runtime_error("Unexpected USearch slot allocation order.");
        }

        live_slots_[seq_id] = result.slot;
    }

    void mark_deleted(uint32_t seq_id) override {
        std::unique_lock lock(state_mutex_);
        auto live_it = live_slots_.find(seq_id);
        if (live_it == live_slots_.end()) {
            return;
        }

        auto const slot = live_it->second;
        live_slots_.erase(live_it);
        if (!tombstones_[slot]) {
            tombstones_[slot] = true;
            free_slots_.push_back(slot);
        }
    }

    std::optional<std::vector<float>> get_vector(uint32_t seq_id) const override {
        std::shared_lock lock(state_mutex_);
        auto live_it = live_slots_.find(seq_id);
        if (live_it == live_slots_.end()) {
            return std::nullopt;
        }

        return slot_vectors_[live_it->second];
    }

    std::vector<vector_index_search_result_t> search(const std::vector<float>& query, size_t k, uint32_t ef,
                                                     const vector_index_filter_t& filter) const override {
        if (query.size() != num_dim() || k == 0) {
            return {};
        }

        std::vector<float> normalized_query;
        const float* query_ptr = prepare_query(*this, query, normalized_query);
        std::shared_lock lock(state_mutex_);
        metric_proxy_t metric{&slot_vectors_, &distance_metric_};
        auto const thread_slot = current_thread_slot();
        usearch::index_search_config_t search_config;
        search_config.thread = thread_slot;
        search_config.expansion = std::max<std::size_t>(ef, k);

        auto matches = index_.search(
            query_ptr,
            k,
            metric,
            search_config,
            [&](auto const& member) {
                auto const slot = metric_proxy_t::slot_for(member);
                if (slot >= tombstones_.size() || tombstones_[slot]) {
                    return false;
                }

                return !filter || filter(static_cast<uint32_t>(member.key));
            });

        std::vector<vector_index_search_result_t> results;
        if (!matches) {
            return results;
        }

        results.reserve(matches.size());
        for (std::size_t i = 0; i != matches.size(); ++i) {
            auto const match = matches[i];
            results.push_back({
                .distance = match.distance,
                .seq_id = static_cast<uint32_t>(match.member.key),
            });
        }
        return results;
    }

    std::optional<float> distance_to_query(uint32_t seq_id, const std::vector<float>& query) const override {
        if (query.size() != num_dim()) {
            return std::nullopt;
        }

        std::vector<float> normalized_query;
        const float* query_ptr = prepare_query(*this, query, normalized_query);
        std::shared_lock lock(state_mutex_);
        auto live_it = live_slots_.find(seq_id);
        if (live_it == live_slots_.end()) {
            return std::nullopt;
        }

        metric_proxy_t metric{&slot_vectors_, &distance_metric_};
        auto const& stored = slot_vectors_[live_it->second];
        return metric.distance(query_ptr, stored.data());
    }

    std::optional<float> distance_between(uint32_t seq_id_i, uint32_t seq_id_j) const override {
        std::shared_lock lock(state_mutex_);
        auto left_it = live_slots_.find(seq_id_i);
        auto right_it = live_slots_.find(seq_id_j);
        if (left_it == live_slots_.end() || right_it == live_slots_.end()) {
            return std::nullopt;
        }

        metric_proxy_t metric{&slot_vectors_, &distance_metric_};
        auto const& lhs = slot_vectors_[left_it->second];
        auto const& rhs = slot_vectors_[right_it->second];
        return metric.distance(lhs.data(), rhs.data());
    }

    vector_index_stats_t stats() const override {
        std::shared_lock lock(state_mutex_);
        return {
            .max_elements = capacity_limit_,
            .current_element_count = index_.size(),
            .deleted_count = free_slots_.size(),
        };
    }

    void repair() override {
        std::unique_lock lock(state_mutex_);
        if (free_slots_.empty()) {
            return;
        }

        auto rebuilt_result = index_t::make(usearch::index_config_t(config_.M));
        if (!rebuilt_result) {
            throw std::runtime_error(rebuilt_result.error.release());
        }

        index_t rebuilt_index = std::move(rebuilt_result.index);
        if (!rebuilt_index.reserve(usearch::index_limits_t(capacity_limit_, thread_count_))) {
            throw std::runtime_error("Unable to reserve rebuilt USearch vector index capacity.");
        }

        std::vector<std::pair<uint32_t, slot_t>> live_entries(live_slots_.begin(), live_slots_.end());
        std::sort(live_entries.begin(), live_entries.end(), [](auto const& lhs, auto const& rhs) {
            return lhs.second < rhs.second;
        });

        std::vector<std::vector<float>> rebuilt_vectors;
        rebuilt_vectors.reserve(capacity_limit_);
        std::vector<std::uint8_t> rebuilt_tombstones;
        rebuilt_tombstones.reserve(capacity_limit_);
        std::unordered_map<uint32_t, slot_t> rebuilt_live_slots;
        rebuilt_live_slots.reserve(live_slots_.size());
        metric_proxy_t rebuilt_metric{&rebuilt_vectors, &distance_metric_};
        auto const update_config = make_update_config(0);

        for (auto const& [seq_id, old_slot]: live_entries) {
            rebuilt_vectors.push_back(slot_vectors_[old_slot]);
            rebuilt_tombstones.push_back(false);
            auto result = rebuilt_index.add(seq_id, rebuilt_vectors.back().data(), rebuilt_metric, update_config);
            if (!result) {
                throw std::runtime_error(result.error.release());
            }
            rebuilt_live_slots[seq_id] = result.slot;
        }

        index_ = std::move(rebuilt_index);
        slot_vectors_ = std::move(rebuilt_vectors);
        tombstones_ = std::move(rebuilt_tombstones);
        live_slots_ = std::move(rebuilt_live_slots);
        free_slots_.clear();
    }

private:
    size_t current_thread_slot() const {
        auto const thread_id = std::this_thread::get_id();
        std::lock_guard lock(thread_slots_mutex_);

        auto existing_it = thread_slots_.find(thread_id);
        if (existing_it != thread_slots_.end()) {
            return existing_it->second;
        }

        auto const next_slot = next_thread_slot_++;
        auto const assigned_slot =
            next_slot < thread_count_ ? next_slot : std::hash<std::thread::id>{}(thread_id) % thread_count_;
        thread_slots_.emplace(thread_id, assigned_slot);
        return assigned_slot;
    }

    usearch::index_update_config_t make_update_config(size_t thread_slot) const {
        usearch::index_update_config_t config;
        config.thread = thread_slot;
        config.expansion = config_.ef_construction;
        return config;
    }

    void ensure_capacity_locked(size_t desired_total_elements) {
        if (desired_total_elements <= capacity_limit_) {
            return;
        }

        auto const scaled_capacity = std::max(
            desired_total_elements,
            static_cast<size_t>(std::ceil(static_cast<double>(desired_total_elements) * 1.3)));
        if (!index_.reserve(usearch::index_limits_t(scaled_capacity, thread_count_))) {
            throw std::runtime_error("Unable to grow USearch vector index capacity.");
        }

        capacity_limit_ = scaled_capacity;
        slot_vectors_.reserve(capacity_limit_);
        tombstones_.reserve(capacity_limit_);
    }

    vector_index_config_t config_;
    size_t thread_count_ = 0;
    size_t capacity_limit_ = 0;
    mutable std::shared_mutex state_mutex_;
    mutable std::mutex thread_slots_mutex_;
    mutable std::unordered_map<std::thread::id, size_t> thread_slots_;
    mutable size_t next_thread_slot_ = 0;
    usearch::metric_punned_t distance_metric_;
    index_t index_;
    std::vector<std::vector<float>> slot_vectors_;
    std::vector<std::uint8_t> tombstones_;
    std::unordered_map<uint32_t, slot_t> live_slots_;
    std::vector<slot_t> free_slots_;
};

}  // namespace

vector_index_t::vector_index_t(size_t num_dim, vector_distance_type_t distance_type):
    num_dim_(num_dim), distance_type_(distance_type) {
}

size_t vector_index_t::num_dim() const noexcept {
    return num_dim_;
}

vector_distance_type_t vector_index_t::distance_type() const noexcept {
    return distance_type_;
}

std::mutex& vector_index_t::lifecycle_mutex() noexcept {
    return lifecycle_mutex_;
}

void vector_index_t::normalize_vector(const std::vector<float>& src, std::vector<float>& norm_dest) {
    float norm = 0.0f;
    for (float value: src) {
        norm += value * value;
    }

    norm = 1.0f / (sqrtf(norm) + 1e-30f);
    for (size_t i = 0; i < src.size(); i++) {
        norm_dest[i] = src[i] * norm;
    }
}

vector_index_t* create_usearch_vector_index(const vector_index_config_t& config) {
    return new usearch_vector_index_t(config);
}
