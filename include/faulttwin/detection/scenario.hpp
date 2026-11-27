#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "faulttwin/features/observation.hpp"

namespace faulttwin::detection {

using features::Observation;
using features::Timestamp;

/// Fault classes of the synthetic scenarios. Each is a ground-truth failure episode.
enum class FaultKind : std::uint8_t {
    kReplicationLagBuildup,  ///< replica replay lag ramps up, then recovers
    kCpuSaturation,          ///< CPU pinned near 100 %
    kOverload,               ///< statement latency and connection utilization rise together
    kNodeDown,               ///< node unreachable (reported down, or telemetry absent)
};
inline constexpr std::size_t kFaultKindCount = 4;
[[nodiscard]] std::string_view to_string(FaultKind kind) noexcept;

/// Benign events that are *not* failures but look like one to a naive threshold.
enum class DistractorKind : std::uint8_t {
    kCpuSpike,      ///< 3-12 s of CPU at 90-99 %
    kLatencySpike,  ///< 3-12 s of high statement latency without connection pressure
    kTelemetryGap,  ///< 1-3.5 s without any telemetry from a node
    kLagBlip,       ///< 1-4 s of replication lag 5-15 s (a large transaction), then decay
    kBusyPeriod,    ///< 30-60 s of legitimate load: CPU 65-82 %, latency x1.6, connections up
};
inline constexpr std::size_t kDistractorKindCount = 5;
[[nodiscard]] std::string_view to_string(DistractorKind kind) noexcept;

struct GroundTruthEpisode {
    std::string node;
    FaultKind kind{FaultKind::kNodeDown};
    Timestamp onset;
    Timestamp end;  ///< the fault is removed at `end`; recovery follows
    double intensity{1.0};
};

struct Distractor {
    std::string node;
    DistractorKind kind{DistractorKind::kCpuSpike};
    Timestamp start;
    Timestamp end;
};

struct ScenarioConfig {
    std::uint64_t seed{1};
    std::chrono::seconds duration{900};
    std::chrono::milliseconds tick{1000};
    std::uint32_t replicas{2};
    std::uint32_t faults{3};
    std::uint32_t distractors{5};
    /// Fault intensity multiplier is drawn uniformly from [min, max]. Lower is harder.
    double intensity_min{0.8};
    double intensity_max{1.2};
    Timestamp start{std::chrono::sys_days{std::chrono::year{2026} / 1 / 1}};
};

/// A generated scenario: observations in event-time order (tick, then node, then metric
/// order), plus the ground truth that produced them. Values are illustrative, not a model of
/// real PostgreSQL behaviour; see docs/phase-reports/phase-04.md §21.
struct Scenario {
    ScenarioConfig config;
    std::vector<std::string> nodes;  ///< "primary", "replica-1", ...
    std::vector<Observation> observations;
    std::vector<GroundTruthEpisode> episodes;
    std::vector<Distractor> distractors;
};

/// Deterministic for a config: an in-tree SplitMix64 PRNG and only +, -, *, / on doubles (no
/// <random> distributions, no libm), compiled with -ffp-contract=off so that no compiler fuses
/// a multiply-add. The observations are therefore bit-identical across compilers, standard
/// libraries and CPU architectures with IEEE-754 doubles.
[[nodiscard]] Scenario generate_scenario(const ScenarioConfig& config);

}  // namespace faulttwin::detection
