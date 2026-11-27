#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "faulttwin/collectors/reading.hpp"
#include "faulttwin/telemetry/telemetry_sample.hpp"

namespace faulttwin::features {

using collectors::Quality;
using telemetry::Timestamp;

/// The node-level signals that Phase 4 turns into features. The set is deliberately small: it
/// is what the default rules and the Phase 7 model need first. Adding a metric is a schema
/// change (FeatureSchema::version), never a silent one.
enum class MetricId : std::uint8_t {
    kAvailability,           ///< 1 = reachable this cycle; down nodes yield kMissing, never 0
    kProbeLatencyMs,         ///< client round trip of SELECT 1
    kReplicationLagSeconds,  ///< replica: primary-side replay lag, or 0 when caught up
    kReplicationLagBytes,    ///< replica: primary's current LSN - replica replay LSN
    kConnectionUtilization,  ///< client backends / max_connections × 100
    kCommitsPerSecond,       ///< transaction throughput
    kStatementLatencyMs,     ///< mean server execution time per statement over the interval
    kCpuPercent,
    kMemoryPercent,
    kDiskPercent,
};

inline constexpr std::size_t kMetricCount = 10;
inline constexpr std::array<MetricId, kMetricCount> kAllMetrics{
    MetricId::kAvailability,          MetricId::kProbeLatencyMs,
    MetricId::kReplicationLagSeconds, MetricId::kReplicationLagBytes,
    MetricId::kConnectionUtilization, MetricId::kCommitsPerSecond,
    MetricId::kStatementLatencyMs,    MetricId::kCpuPercent,
    MetricId::kMemoryPercent,         MetricId::kDiskPercent,
};

/// Stable snake_case name used in schemas, rules, reports and fixtures.
[[nodiscard]] std::string_view to_string(MetricId metric) noexcept;
[[nodiscard]] std::optional<MetricId> metric_from_string(std::string_view name) noexcept;

/// One value of one metric of one node at one event time (the time it was measured, not the
/// time it arrived). Quality kOk carries a finite value; kStale keeps the old value for context
/// but is treated like missing data by feature extraction; kMissing / kInvalid carry no value;
/// kNotApplicable observations are ignored (they are neither data nor missing data).
struct Observation {
    std::string node;
    MetricId metric{MetricId::kAvailability};
    Timestamp at;
    Quality quality{Quality::kMissing};
    double value{0.0};

    [[nodiscard]] static Observation ok(std::string node, MetricId metric, Timestamp at,
                                        double value) {
        return {std::move(node), metric, at, Quality::kOk, value};
    }
    [[nodiscard]] static Observation missing(std::string node, MetricId metric, Timestamp at,
                                             Quality quality = Quality::kMissing) {
        return {std::move(node), metric, at, quality, 0.0};
    }
};

}  // namespace faulttwin::features
