#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

#include "field.h"

struct vector_index_search_result_t {
    float distance;
    uint32_t seq_id;
};

struct vector_index_stats_t {
    size_t max_elements = 0;
    size_t current_element_count = 0;
    size_t deleted_count = 0;
};

using vector_index_filter_t = std::function<bool(uint32_t)>;

struct vector_index_config_t {
    size_t num_dim;
    size_t init_size;
    vector_distance_type_t distance_type;
    size_t M = 16;
    size_t ef_construction = 200;
};

class vector_index_t {
public:
    vector_index_t(size_t num_dim, vector_distance_type_t distance_type);
    virtual ~vector_index_t() = default;

    size_t num_dim() const noexcept;
    vector_distance_type_t distance_type() const noexcept;
    std::mutex& lifecycle_mutex() noexcept;

    static void normalize_vector(const std::vector<float>& src, std::vector<float>& norm_dest);

    virtual void ensure_capacity(size_t desired_total_elements) = 0;
    virtual void add_vector(uint32_t seq_id, const std::vector<float>& values) = 0;
    virtual void mark_deleted(uint32_t seq_id) = 0;
    virtual std::optional<std::vector<float>> get_vector(uint32_t seq_id) const = 0;
    virtual std::vector<vector_index_search_result_t> search(const std::vector<float>& query, size_t k, uint32_t ef,
                                                             const vector_index_filter_t& filter) const = 0;
    virtual std::optional<float> distance_to_query(uint32_t seq_id, const std::vector<float>& query) const = 0;
    virtual std::optional<float> distance_between(uint32_t seq_id_i, uint32_t seq_id_j) const = 0;
    virtual vector_index_stats_t stats() const = 0;
    virtual void repair() = 0;

private:
    size_t num_dim_;
    vector_distance_type_t distance_type_;
    mutable std::mutex lifecycle_mutex_;
};

vector_index_t* create_usearch_vector_index(const vector_index_config_t& config);
