#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include <usearch/index.hpp>
#include <usearch/index_dense.hpp>
#include <usearch/index_plugins.hpp>

namespace {

using namespace unum::usearch;

TEST(USearchDensePrototype, SupportsStoredVectorsDeleteAndFilteredSearch) {
    metric_punned_t metric(1, metric_kind_t::l2sq_k, scalar_kind_t::f32_k);
    index_dense_config_t config(/*connectivity=*/16, /*expansion_add=*/64, /*expansion_search=*/32);
    auto index_result = index_dense_t::make(metric, config);
    ASSERT_TRUE(index_result);

    index_dense_t index = std::move(index_result.index);
    ASSERT_TRUE(index.try_reserve(3));

    const float value_42[] = {1.0f};
    const float value_43[] = {2.0f};
    const float value_44[] = {3.0f};
    const float replacement_43[] = {1.2f};
    const float query[] = {1.1f};

    ASSERT_TRUE(index.add(42, value_42));
    ASSERT_TRUE(index.add(43, value_43));
    ASSERT_TRUE(index.add(44, value_44));

    auto initial_results = index.search(query, 3);
    ASSERT_TRUE(initial_results);
    ASSERT_EQ(initial_results.size(), 3u);
    EXPECT_EQ(initial_results[0].member.key, static_cast<index_dense_t::key_t>(42));
    EXPECT_EQ(initial_results[1].member.key, static_cast<index_dense_t::key_t>(43));
    EXPECT_EQ(initial_results[2].member.key, static_cast<index_dense_t::key_t>(44));

    auto filtered_results = index.filtered_search(query, 3, [](index_dense_t::key_t key) {
        return key != static_cast<index_dense_t::key_t>(42);
    });
    ASSERT_TRUE(filtered_results);
    ASSERT_EQ(filtered_results.size(), 2u);
    EXPECT_EQ(filtered_results[0].member.key, static_cast<index_dense_t::key_t>(43));
    EXPECT_EQ(filtered_results[1].member.key, static_cast<index_dense_t::key_t>(44));

    ASSERT_TRUE(index.remove(43));

    float recovered = 0.0f;
    EXPECT_EQ(index.get(43, &recovered), 0u);

    ASSERT_TRUE(index.add(43, replacement_43));
    ASSERT_EQ(index.get(43, &recovered), 1u);
    EXPECT_FLOAT_EQ(recovered, replacement_43[0]);

    auto refreshed_results = index.search(query, 3);
    ASSERT_TRUE(refreshed_results);
    ASSERT_EQ(refreshed_results.size(), 3u);
    EXPECT_EQ(refreshed_results[0].member.key, static_cast<index_dense_t::key_t>(43));
    EXPECT_EQ(refreshed_results[1].member.key, static_cast<index_dense_t::key_t>(42));
    EXPECT_EQ(refreshed_results[2].member.key, static_cast<index_dense_t::key_t>(44));
}

TEST(USearchCorePrototype, SupportsPerQueryExpansionWithExternalVectorStorage) {
    using key_t = std::uint32_t;
    using index_t = index_gt<float, key_t, std::uint32_t>;
    using member_cref_t = typename index_t::member_cref_t;
    using member_citerator_t = typename index_t::member_citerator_t;

    constexpr std::size_t kDimensions = 32;
    constexpr std::size_t kVectorCount = 256;
    constexpr std::size_t kQueryCount = 16;

    struct metric_t {
        std::vector<std::array<float, kDimensions>> const* vectors = nullptr;

        float operator()(float const* query, member_cref_t const& member) const {
            return metric_cos_gt<float, float>{}(query, (*vectors)[get_slot(member)].data(), kDimensions);
        }

        float operator()(member_cref_t const& a, member_cref_t const& b) const {
            return metric_cos_gt<float, float>{}((*vectors)[get_slot(a)].data(), (*vectors)[get_slot(b)].data(), kDimensions);
        }

        float operator()(float const* query, member_citerator_t const& member) const {
            return metric_cos_gt<float, float>{}(query, (*vectors)[get_slot(member)].data(), kDimensions);
        }

        float operator()(member_citerator_t const& a, member_citerator_t const& b) const {
            return metric_cos_gt<float, float>{}((*vectors)[get_slot(a)].data(), (*vectors)[get_slot(b)].data(), kDimensions);
        }
    };

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> distribution(0.0f, 1.0f);
    std::vector<std::array<float, kDimensions>> vectors(kVectorCount);
    for (auto& vector : vectors) {
        for (auto& value : vector) {
            value = distribution(rng);
        }
    }

    index_config_t config(/*connectivity=*/16);
    auto index_result = index_t::make(config);
    ASSERT_TRUE(index_result);
    index_t index = std::move(index_result.index);

    ASSERT_TRUE(index.reserve(index_limits_t(kVectorCount, 1)));

    metric_t metric{&vectors};
    index_update_config_t add_config;
    add_config.thread = 0;
    add_config.expansion = 32;

    for (std::size_t i = 0; i < vectors.size(); ++i) {
        ASSERT_TRUE(index.add(static_cast<key_t>(i), vectors[i].data(), metric, add_config));
    }

    index_search_config_t low_expansion_config;
    low_expansion_config.thread = 0;
    low_expansion_config.expansion = 8;

    index_search_config_t high_expansion_config;
    high_expansion_config.thread = 0;
    high_expansion_config.expansion = 128;

    std::size_t total_low_visited = 0;
    std::size_t total_high_visited = 0;
    std::size_t total_low_distances = 0;
    std::size_t total_high_distances = 0;

    for (std::size_t i = 0; i < kQueryCount; ++i) {
        auto low_results = index.search(vectors[i].data(), 10, metric, low_expansion_config);
        auto high_results = index.search(vectors[i].data(), 10, metric, high_expansion_config);

        ASSERT_TRUE(low_results);
        ASSERT_TRUE(high_results);
        ASSERT_FALSE(low_results.empty());
        ASSERT_FALSE(high_results.empty());
        EXPECT_TRUE(low_results.contains(static_cast<key_t>(i)));
        EXPECT_TRUE(high_results.contains(static_cast<key_t>(i)));

        total_low_visited += low_results.visited_members;
        total_high_visited += high_results.visited_members;
        total_low_distances += low_results.computed_distances;
        total_high_distances += high_results.computed_distances;
    }

    EXPECT_GE(total_high_visited, total_low_visited);
    EXPECT_GE(total_high_distances, total_low_distances);

    auto filtered_results = index.search(
        vectors[0].data(),
        10,
        metric,
        high_expansion_config,
        [](member_cref_t const& member) {
            return member.key != 0;
        });

    ASSERT_TRUE(filtered_results);
    EXPECT_FALSE(filtered_results.contains(0));
}

}  // namespace
