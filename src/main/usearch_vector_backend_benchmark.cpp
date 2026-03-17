#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <usearch/index_plugins.hpp>

#include "vector_index.h"

namespace {

namespace usearch = unum::usearch;

struct benchmark_config_t {
    std::size_t docs = 20000;
    std::size_t dims = 384;
    std::size_t cycles = 10;
    std::size_t updates_per_cycle = 400;
    std::size_t replacements_per_cycle = 100;
    std::size_t searches_per_cycle = 200;
    std::size_t k = 20;
    std::size_t ef = 80;
    std::size_t kernel_samples = 4096;
    std::size_t kernel_repeats = 64;
    std::uint32_t seed = 42;
};

struct timed_measurement_t {
    double elapsed_ms = 0.0;
    double p50_us = 0.0;
    double p95_us = 0.0;
    double per_sec = 0.0;
};

struct kernel_measurement_t {
    std::string name;
    double elapsed_ms = 0.0;
    double relative_to_scalar_pct = 0.0;
    double checksum = 0.0;
};

struct benchmark_result_t {
    std::vector<kernel_measurement_t> kernel_measurements;
    std::string kernel_winner;
    double initial_index_ms = 0.0;
    timed_measurement_t write_measurement;
    timed_measurement_t search_measurement;
    std::size_t total_update_ops = 0;
    std::size_t total_replacement_ops = 0;
    std::size_t total_search_ops = 0;
    std::size_t final_max_elements = 0;
    std::size_t final_current_element_count = 0;
    std::size_t final_deleted_count = 0;
    std::size_t search_checksum = 0;
};

void print_usage(char const* argv0) {
    std::cerr
        << "Usage: " << argv0 << " [options]\n"
        << "Options:\n"
        << "  --docs N                   Initial live vector count (default: 20000)\n"
        << "  --dims N                   Vector dimensions (default: 384)\n"
        << "  --cycles N                 Mixed workload cycles (default: 10)\n"
        << "  --updates-per-cycle N      In-place upserts per cycle (default: 400)\n"
        << "  --replacements-per-cycle N Delete+insert replacements per cycle (default: 100)\n"
        << "  --searches-per-cycle N     Searches per cycle (default: 200)\n"
        << "  --k N                      Search top-k (default: 20)\n"
        << "  --ef N                     Search expansion (default: 80)\n"
        << "  --kernel-samples N         Distance-kernel sample count (default: 4096)\n"
        << "  --kernel-repeats N         Distance-kernel repeat count (default: 64)\n"
        << "  --seed N                   RNG seed (default: 42)\n"
        << "  --help                     Show this help\n";
}

std::size_t parse_size(char const* value, char const* flag) {
    try {
        return static_cast<std::size_t>(std::stoull(value));
    } catch (...) {
        throw std::invalid_argument(std::string("Invalid value for ") + flag + ": " + value);
    }
}

benchmark_config_t parse_args(int argc, char** argv) {
    benchmark_config_t config;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        auto require_value = [&](char const* flag) -> char const* {
            if (i + 1 >= argc) {
                throw std::invalid_argument(std::string("Missing value for ") + flag);
            }
            return argv[++i];
        };

        if (arg == "--docs") {
            config.docs = parse_size(require_value("--docs"), "--docs");
        } else if (arg == "--dims") {
            config.dims = parse_size(require_value("--dims"), "--dims");
        } else if (arg == "--cycles") {
            config.cycles = parse_size(require_value("--cycles"), "--cycles");
        } else if (arg == "--updates-per-cycle") {
            config.updates_per_cycle = parse_size(require_value("--updates-per-cycle"), "--updates-per-cycle");
        } else if (arg == "--replacements-per-cycle") {
            config.replacements_per_cycle =
                parse_size(require_value("--replacements-per-cycle"), "--replacements-per-cycle");
        } else if (arg == "--searches-per-cycle") {
            config.searches_per_cycle = parse_size(require_value("--searches-per-cycle"), "--searches-per-cycle");
        } else if (arg == "--k") {
            config.k = parse_size(require_value("--k"), "--k");
        } else if (arg == "--ef") {
            config.ef = parse_size(require_value("--ef"), "--ef");
        } else if (arg == "--kernel-samples") {
            config.kernel_samples = parse_size(require_value("--kernel-samples"), "--kernel-samples");
        } else if (arg == "--kernel-repeats") {
            config.kernel_repeats = parse_size(require_value("--kernel-repeats"), "--kernel-repeats");
        } else if (arg == "--seed") {
            config.seed = static_cast<std::uint32_t>(parse_size(require_value("--seed"), "--seed"));
        } else if (arg == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            throw std::invalid_argument(std::string("Unknown option: ") + std::string(arg));
        }
    }

    if (config.docs == 0 || config.dims == 0 || config.k == 0 || config.ef == 0) {
        throw std::invalid_argument("docs, dims, k, and ef must all be greater than zero");
    }

    return config;
}

std::vector<float> make_random_vector(std::size_t dims, std::mt19937& rng) {
    std::uniform_real_distribution<float> distribution(0.0f, 1.0f);
    std::vector<float> values(dims);
    for (auto& value: values) {
        value = distribution(rng);
    }
    return values;
}

std::vector<float> normalize_copy(std::vector<float> values) {
    std::vector<float> normalized(values.size());
    vector_index_t::normalize_vector(values, normalized);
    return normalized;
}

double percentile_us(std::vector<double> values, double percentile) {
    if (values.empty()) {
        return 0.0;
    }

    std::sort(values.begin(), values.end());
    auto const raw_index = percentile * static_cast<double>(values.size() - 1);
    auto const index = static_cast<std::size_t>(raw_index);
    return values[index];
}

float scalar_inner_product_distance(float const* lhs, float const* rhs, std::size_t dims) noexcept {
    float dot = 0.0f;
    for (std::size_t i = 0; i != dims; ++i) {
        dot += lhs[i] * rhs[i];
    }
    return 1.0f - dot;
}

kernel_measurement_t run_kernel_case(std::string name,
                                     std::vector<std::vector<float>> const& lhs_vectors,
                                     std::vector<std::vector<float>> const& rhs_vectors,
                                     std::size_t repeats,
                                     std::optional<usearch::metric_punned_t> const& builtin_metric) {
    auto const started_at = std::chrono::steady_clock::now();
    double checksum = 0.0;

    for (std::size_t repeat = 0; repeat != repeats; ++repeat) {
        for (std::size_t i = 0; i != lhs_vectors.size(); ++i) {
            auto const& lhs = lhs_vectors[i];
            auto const& rhs = rhs_vectors[i];
            if (builtin_metric.has_value()) {
                checksum += builtin_metric.value()(reinterpret_cast<usearch::byte_t const*>(lhs.data()),
                                                   reinterpret_cast<usearch::byte_t const*>(rhs.data()));
            } else {
                checksum += scalar_inner_product_distance(lhs.data(), rhs.data(), lhs.size());
            }
        }
    }

    auto const elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started_at).count();
    return {
        .name = std::move(name),
        .elapsed_ms = elapsed_ms,
        .checksum = checksum,
    };
}

std::vector<kernel_measurement_t> run_kernel_benchmark(benchmark_config_t const& config) {
    std::mt19937 rng(config.seed);
    std::vector<std::vector<float>> raw_queries;
    std::vector<std::vector<float>> raw_vectors;
    raw_queries.reserve(config.kernel_samples);
    raw_vectors.reserve(config.kernel_samples);
    for (std::size_t i = 0; i != config.kernel_samples; ++i) {
        raw_queries.push_back(make_random_vector(config.dims, rng));
        raw_vectors.push_back(make_random_vector(config.dims, rng));
    }

    std::vector<std::vector<float>> normalized_queries;
    std::vector<std::vector<float>> normalized_vectors;
    normalized_queries.reserve(config.kernel_samples);
    normalized_vectors.reserve(config.kernel_samples);
    for (std::size_t i = 0; i != config.kernel_samples; ++i) {
        normalized_queries.push_back(normalize_copy(raw_queries[i]));
        normalized_vectors.push_back(normalize_copy(raw_vectors[i]));
    }

    auto scalar_case = run_kernel_case("parity_scalar_normalized_ip", normalized_queries, normalized_vectors,
                                       config.kernel_repeats, std::nullopt);
    auto usearch_ip_case = run_kernel_case(
        "usearch_builtin_ip_normalized", normalized_queries, normalized_vectors, config.kernel_repeats,
        usearch::metric_punned_t(config.dims, usearch::metric_kind_t::ip_k, usearch::scalar_kind_t::f32_k));
    auto usearch_cos_case = run_kernel_case(
        "usearch_builtin_cos_raw", raw_queries, raw_vectors, config.kernel_repeats,
        usearch::metric_punned_t(config.dims, usearch::metric_kind_t::cos_k, usearch::scalar_kind_t::f32_k));

    std::vector<kernel_measurement_t> results;
    results.push_back(std::move(scalar_case));
    results.push_back(std::move(usearch_ip_case));
    results.push_back(std::move(usearch_cos_case));

    auto const scalar_elapsed = results.front().elapsed_ms;
    for (auto& result: results) {
        result.relative_to_scalar_pct = scalar_elapsed == 0.0
                                            ? 0.0
                                            : ((scalar_elapsed - result.elapsed_ms) / scalar_elapsed) * 100.0;
    }

    std::sort(results.begin(), results.end(), [](auto const& lhs, auto const& rhs) {
        return lhs.elapsed_ms < rhs.elapsed_ms;
    });
    return results;
}

timed_measurement_t summarize_latencies(std::vector<double> const& latencies_us) {
    timed_measurement_t summary;
    if (latencies_us.empty()) {
        return summary;
    }

    auto const total_us = std::accumulate(latencies_us.begin(), latencies_us.end(), 0.0);
    summary.elapsed_ms = total_us / 1000.0;
    summary.p50_us = percentile_us(latencies_us, 0.50);
    summary.p95_us = percentile_us(latencies_us, 0.95);
    summary.per_sec = total_us == 0.0 ? 0.0 : (static_cast<double>(latencies_us.size()) * 1000000.0) / total_us;
    return summary;
}

benchmark_result_t run_mixed_workload(benchmark_config_t const& config) {
    benchmark_result_t result;
    result.kernel_measurements = run_kernel_benchmark(config);
    result.kernel_winner = result.kernel_measurements.front().name;

    std::mt19937 rng(config.seed + 1);
    std::uniform_int_distribution<std::uint32_t> live_pick;

    auto index = std::unique_ptr<vector_index_t>(create_usearch_vector_index({
        .num_dim = config.dims,
        .init_size = config.docs,
        .distance_type = cosine,
        .M = 16,
        .ef_construction = 200,
    }));
    index->ensure_capacity(config.docs);

    std::vector<std::uint32_t> live_ids;
    live_ids.reserve(config.docs);
    std::unordered_map<std::uint32_t, std::vector<float>> live_vectors;
    live_vectors.reserve(config.docs + config.cycles * config.replacements_per_cycle);

    auto const index_started_at = std::chrono::steady_clock::now();
    std::uint32_t next_id = 0;
    for (std::size_t i = 0; i != config.docs; ++i) {
        auto values = make_random_vector(config.dims, rng);
        live_ids.push_back(next_id);
        live_vectors.emplace(next_id, values);
        index->add_vector(next_id, values);
        ++next_id;
    }
    result.initial_index_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - index_started_at).count();

    std::vector<double> write_latencies_us;
    std::vector<double> search_latencies_us;
    write_latencies_us.reserve(config.cycles * (config.updates_per_cycle + config.replacements_per_cycle));
    search_latencies_us.reserve(config.cycles * config.searches_per_cycle);

    vector_index_filter_t even_filter = [](std::uint32_t seq_id) {
        return (seq_id % 2) == 0;
    };

    for (std::size_t cycle = 0; cycle != config.cycles; ++cycle) {
        auto const write_ops = config.updates_per_cycle + config.replacements_per_cycle;
        auto const max_ops = std::max(write_ops, config.searches_per_cycle);

        for (std::size_t op = 0; op != max_ops; ++op) {
            if (op < config.replacements_per_cycle) {
                live_pick = std::uniform_int_distribution<std::uint32_t>(
                    0, static_cast<std::uint32_t>(live_ids.size() - 1));
                auto const victim_position = static_cast<std::size_t>(live_pick(rng));
                auto const victim_id = live_ids[victim_position];
                auto replacement = make_random_vector(config.dims, rng);

                auto const started_at = std::chrono::steady_clock::now();
                index->mark_deleted(victim_id);
                live_vectors.erase(victim_id);
                live_ids[victim_position] = next_id;
                live_vectors.emplace(next_id, replacement);
                index->add_vector(next_id, replacement);
                auto const elapsed_us = std::chrono::duration<double, std::micro>(
                    std::chrono::steady_clock::now() - started_at).count();
                write_latencies_us.push_back(elapsed_us);
                ++next_id;
                ++result.total_replacement_ops;
            } else if (op < write_ops) {
                live_pick = std::uniform_int_distribution<std::uint32_t>(
                    0, static_cast<std::uint32_t>(live_ids.size() - 1));
                auto const live_id = live_ids[static_cast<std::size_t>(live_pick(rng))];
                auto updated = make_random_vector(config.dims, rng);

                auto const started_at = std::chrono::steady_clock::now();
                live_vectors[live_id] = updated;
                index->add_vector(live_id, updated);
                auto const elapsed_us = std::chrono::duration<double, std::micro>(
                    std::chrono::steady_clock::now() - started_at).count();
                write_latencies_us.push_back(elapsed_us);
                ++result.total_update_ops;
            }

            if (op < config.searches_per_cycle) {
                live_pick = std::uniform_int_distribution<std::uint32_t>(
                    0, static_cast<std::uint32_t>(live_ids.size() - 1));
                auto const query_id = live_ids[static_cast<std::size_t>(live_pick(rng))];
                auto const& query = live_vectors.at(query_id);
                auto const filter = (op % 2) == 0 ? even_filter : vector_index_filter_t{};

                auto const started_at = std::chrono::steady_clock::now();
                auto matches = index->search(query, config.k, config.ef, filter);
                auto const elapsed_us = std::chrono::duration<double, std::micro>(
                    std::chrono::steady_clock::now() - started_at).count();
                search_latencies_us.push_back(elapsed_us);
                ++result.total_search_ops;
                result.search_checksum += matches.size();
                if (!matches.empty()) {
                    result.search_checksum += matches.front().seq_id;
                }
            }
        }
    }

    auto const final_stats = index->stats();
    result.write_measurement = summarize_latencies(write_latencies_us);
    result.search_measurement = summarize_latencies(search_latencies_us);
    result.final_max_elements = final_stats.max_elements;
    result.final_current_element_count = final_stats.current_element_count;
    result.final_deleted_count = final_stats.deleted_count;
    return result;
}

void print_result(benchmark_config_t const& config, benchmark_result_t const& result) {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "USearch vector backend benchmark\n";
    std::cout << "config.docs=" << config.docs << '\n';
    std::cout << "config.dims=" << config.dims << '\n';
    std::cout << "config.cycles=" << config.cycles << '\n';
    std::cout << "config.updates_per_cycle=" << config.updates_per_cycle << '\n';
    std::cout << "config.replacements_per_cycle=" << config.replacements_per_cycle << '\n';
    std::cout << "config.searches_per_cycle=" << config.searches_per_cycle << '\n';
    std::cout << "config.k=" << config.k << '\n';
    std::cout << "config.ef=" << config.ef << '\n';
    std::cout << "config.seed=" << config.seed << '\n';
    std::cout << '\n';

    std::cout << "[kernel]\n";
    for (auto const& measurement: result.kernel_measurements) {
        std::cout << "kernel." << measurement.name << ".ms=" << measurement.elapsed_ms << '\n';
        std::cout << "kernel." << measurement.name << ".vs_scalar_pct=" << measurement.relative_to_scalar_pct << '\n';
        std::cout << "kernel." << measurement.name << ".checksum=" << measurement.checksum << '\n';
    }
    std::cout << "kernel.winner=" << result.kernel_winner << '\n';
    std::cout << '\n';

    std::cout << "[mixed]\n";
    std::cout << "mixed.initial_index_ms=" << result.initial_index_ms << '\n';
    std::cout << "mixed.update_ops=" << result.total_update_ops << '\n';
    std::cout << "mixed.replacement_ops=" << result.total_replacement_ops << '\n';
    std::cout << "mixed.search_ops=" << result.total_search_ops << '\n';
    std::cout << "mixed.write_ms=" << result.write_measurement.elapsed_ms << '\n';
    std::cout << "mixed.write_p50_us=" << result.write_measurement.p50_us << '\n';
    std::cout << "mixed.write_p95_us=" << result.write_measurement.p95_us << '\n';
    std::cout << "mixed.write_ops_per_sec=" << result.write_measurement.per_sec << '\n';
    std::cout << "mixed.search_ms=" << result.search_measurement.elapsed_ms << '\n';
    std::cout << "mixed.search_p50_us=" << result.search_measurement.p50_us << '\n';
    std::cout << "mixed.search_p95_us=" << result.search_measurement.p95_us << '\n';
    std::cout << "mixed.search_ops_per_sec=" << result.search_measurement.per_sec << '\n';
    std::cout << "mixed.final_max_elements=" << result.final_max_elements << '\n';
    std::cout << "mixed.final_current_element_count=" << result.final_current_element_count << '\n';
    std::cout << "mixed.final_deleted_count=" << result.final_deleted_count << '\n';
    std::cout << "mixed.search_checksum=" << result.search_checksum << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    try {
        auto const config = parse_args(argc, argv);
        auto const result = run_mixed_workload(config);
        print_result(config, result);
        return 0;
    } catch (std::exception const& error) {
        std::cerr << error.what() << '\n';
        print_usage(argv[0]);
        return 1;
    }
}
