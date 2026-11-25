#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "faulttwin/telemetry/node_id.hpp"
#include "faulttwin/telemetry/node_role.hpp"

namespace faulttwin::telemetry {

using Clock = std::chrono::system_clock;
using Timestamp = Clock::time_point;

/// Untrusted telemetry exactly as a collector (or the synthetic generator)
/// produced it. Any field may hold any value; nothing here is validated.
struct RawTelemetrySample {
    std::string node_id;
    NodeRole role{NodeRole::kUnknown};
    Timestamp collected_at;  ///< wall-clock collection time (UTC); defaults to the epoch ("unset")
    double cpu_usage_percent{0.0};
    double memory_usage_percent{0.0};
    double disk_usage_percent{0.0};
    /// std::nullopt means "not measured". It never means zero lag.
    std::optional<double> replication_lag_ms;
};

/// Interpretation of the replication lag field for a validated sample.
enum class ReplicationLagState : std::uint8_t {
    kNotApplicable,  ///< primary or unknown role: lag has no meaning
    kNotMeasured,    ///< replica, but no measurement was available
    kMeasured,       ///< replica with a finite, non-negative measurement
};

[[nodiscard]] std::string_view to_string(ReplicationLagState state) noexcept;

namespace detail {
struct SampleFactory;  // defined only in validation.cpp
}

/// A telemetry sample that passed validate_sample(). Immutable and valid by
/// construction: there is no public constructor.
class TelemetrySample {
  public:
    [[nodiscard]] const NodeId& node_id() const noexcept { return node_id_; }
    [[nodiscard]] NodeRole role() const noexcept { return role_; }
    [[nodiscard]] Timestamp collected_at() const noexcept { return collected_at_; }
    [[nodiscard]] double cpu_usage_percent() const noexcept { return cpu_usage_percent_; }
    [[nodiscard]] double memory_usage_percent() const noexcept { return memory_usage_percent_; }
    [[nodiscard]] double disk_usage_percent() const noexcept { return disk_usage_percent_; }
    [[nodiscard]] std::optional<double> replication_lag_ms() const noexcept {
        return replication_lag_ms_;
    }
    [[nodiscard]] ReplicationLagState replication_lag_state() const noexcept;

  private:
    friend struct detail::SampleFactory;

    TelemetrySample(NodeId node_id, NodeRole role, Timestamp collected_at, double cpu,
                    double memory, double disk, std::optional<double> replication_lag_ms)
        : node_id_(std::move(node_id)),
          role_(role),
          collected_at_(collected_at),
          cpu_usage_percent_(cpu),
          memory_usage_percent_(memory),
          disk_usage_percent_(disk),
          replication_lag_ms_(replication_lag_ms) {}

    NodeId node_id_;
    NodeRole role_;
    Timestamp collected_at_;
    double cpu_usage_percent_;
    double memory_usage_percent_;
    double disk_usage_percent_;
    std::optional<double> replication_lag_ms_;
};

}  // namespace faulttwin::telemetry
