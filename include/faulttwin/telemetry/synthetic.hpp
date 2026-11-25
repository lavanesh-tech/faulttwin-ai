#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "faulttwin/telemetry/telemetry_sample.hpp"
#include "faulttwin/telemetry/validation.hpp"

namespace faulttwin::telemetry {

/// Defects the synthetic generator can inject. Each maps to exactly one
/// expected validation issue, which gives the CLI a ground truth to check the
/// validator against.
enum class InjectedDefect : std::uint8_t {
    kEmptyNodeId,
    kOverlongNodeId,
    kInvalidNodeIdCharacter,
    kInvalidRole,
    kUnsetTimestamp,
    kFutureTimestamp,
    kCpuOutOfRange,
    kMemoryNaN,
    kDiskInfinity,
    kNegativeReplicationLag,
    kReplicationLagOnPrimary,
};

inline constexpr std::size_t kInjectedDefectCount = 11;

[[nodiscard]] std::string_view to_string(InjectedDefect defect) noexcept;
[[nodiscard]] ValidationIssue expected_issue(InjectedDefect defect) noexcept;

struct SyntheticConfig {
    std::uint32_t node_count{3};  ///< node 0 is the primary, the rest are replicas
    std::uint32_t samples_per_node{5};
    std::uint64_t seed{42};
    double invalid_fraction{0.0};              ///< probability in [0, 1]
    double replica_lag_missing_fraction{0.1};  ///< probability in [0, 1]
    Timestamp start_time{std::chrono::sys_days{std::chrono::year{2026} / 1 / 1}};
    std::chrono::milliseconds interval{1000};
};

struct SyntheticSample {
    RawTelemetrySample raw;
    std::optional<InjectedDefect> defect;  ///< ground truth: nullopt means "should be valid"
};

/// Deterministic generator: the same SyntheticConfig always yields the same
/// sequence on every platform (it uses an in-tree PRNG and does not use
/// <random> distributions, whose output is implementation-defined).
///
/// Values are illustrative only; they are not a model of real PostgreSQL load.
[[nodiscard]] std::vector<SyntheticSample> generate_synthetic_samples(
    const SyntheticConfig& config);

/// The reference "now" used to validate generated samples: the last tick.
[[nodiscard]] Timestamp synthetic_reference_now(const SyntheticConfig& config) noexcept;

}  // namespace faulttwin::telemetry
