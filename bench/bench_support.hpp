#pragma once

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "faulttwin/telemetry/synthetic.hpp"
#include "faulttwin/telemetry/telemetry_sample.hpp"

namespace faulttwin::bench {

/// Experiment 1 workload: 64 nodes x 15,625 ticks = 1,000,000 events, fixed
/// seed, 1% injected defects (so the invalid path is exercised).
inline constexpr std::uint32_t kWorkloadNodes = 64;
inline constexpr std::uint32_t kWorkloadSamplesPerNode = 15'625;
inline constexpr std::uint64_t kWorkloadSeed = 20'261'125;
inline constexpr double kWorkloadInvalidFraction = 0.01;

inline telemetry::SyntheticConfig workload_config() {
    telemetry::SyntheticConfig config;
    config.node_count = kWorkloadNodes;
    config.samples_per_node = kWorkloadSamplesPerNode;
    config.seed = kWorkloadSeed;
    config.invalid_fraction = kWorkloadInvalidFraction;
    return config;
}

/// Generated once per process; every benchmark copies from it.
inline const std::vector<telemetry::RawTelemetrySample>& workload() {
    static const std::vector<telemetry::RawTelemetrySample> raws = [] {
        const auto samples = telemetry::generate_synthetic_samples(workload_config());
        std::vector<telemetry::RawTelemetrySample> out;
        out.reserve(samples.size());
        for (const auto& s : samples) {
            out.push_back(s.raw);
        }
        return out;
    }();
    return raws;
}

/// "now" for validation: the workload's last tick, so no sample is in the future.
inline telemetry::Timestamp workload_now() {
    return telemetry::synthetic_reference_now(workload_config());
}

/// Splits the workload by node, round-robin, like one collector per node set.
/// The generator is tick-major, so sample i belongs to node i % kWorkloadNodes.
/// Per-producer order is preserved. `limit` caps the total number of events.
inline std::vector<std::vector<telemetry::RawTelemetrySample>> split_by_node(std::size_t producers,
                                                                             std::size_t limit) {
    const auto& all = workload();
    std::vector<std::vector<telemetry::RawTelemetrySample>> parts(producers);
    const std::size_t n = std::min(limit, all.size());
    for (auto& part : parts) {
        part.reserve(n / producers + 1);
    }
    for (std::size_t i = 0; i < n; ++i) {
        parts[(i % kWorkloadNodes) % producers].push_back(all[i]);
    }
    return parts;
}

/// Deterministic CPU work standing in for enrichment/routing that a real sink
/// would do. Identical in the baseline and the pipeline.
inline std::uint64_t simulated_work(std::uint64_t iterations, std::uint64_t seed) {
    std::uint64_t state = seed;
    std::uint64_t acc = 0;
    for (std::uint64_t i = 0; i < iterations; ++i) {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        acc ^= z ^ (z >> 31U);
    }
    benchmark::DoNotOptimize(acc);
    return acc;
}

/// Nearest-rank percentile (docs/evaluation/metric-definitions.md §1).
/// Sorts `values` in place. Returns 0 for an empty input.
inline double nearest_rank(std::vector<std::int64_t>& values, double percentile) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const auto n = static_cast<double>(values.size());
    auto rank = static_cast<std::size_t>(percentile / 100.0 * n + 0.999999999);
    rank = std::clamp<std::size_t>(rank, 1, values.size());
    return static_cast<double>(values[rank - 1]);
}

/// Peak resident set size of this process (VmHWM) in MiB, or -1 if unavailable
/// (for example on macOS, which has no /proc).
inline double peak_rss_mib() {
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
        if (key == "VmHWM:") {
            double kib = 0;
            status >> kib;
            return kib / 1024.0;
        }
        std::string rest;
        std::getline(status, rest);
    }
    return -1.0;
}

}  // namespace faulttwin::bench
