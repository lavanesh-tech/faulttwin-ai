#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "faulttwin/core/time.hpp"

namespace faulttwin::api {

/// What the API reports about one monitored node: the latest values the monitor loop saw,
/// with the same missing-is-not-zero rule as the collectors (nullopt = not known).
struct NodeStatus {
    std::string node;
    std::string role;          ///< "primary", "replica", "unknown"
    std::string availability;  ///< "up", "degraded", "down"
    core::WallTime observed_at;
    std::optional<double> replication_lag_seconds;
    std::optional<double> probe_latency_ms;
};

struct FiringAlert {
    std::string rule_id;
    std::string node;
};

struct ClusterStatus {
    std::uint64_t cycles{0};
    std::optional<core::WallTime> last_cycle_at;  ///< wall time the last cycle finished
    std::string source;                           ///< "postgres" or "synthetic"
    std::vector<NodeStatus> nodes;
    std::vector<FiringAlert> firing;
};

/// The hand-over point between the monitor thread (writer) and API threads (readers). A
/// whole ClusterStatus is replaced at once, so readers never see half of a cycle. Copies are
/// small (a few nodes) and taken under a mutex.
class StatusBoard {
  public:
    void publish(ClusterStatus status) {
        const std::scoped_lock lock(mutex_);
        status_ = std::move(status);
        last_publish_ = std::chrono::steady_clock::now();
    }
    [[nodiscard]] ClusterStatus snapshot() const {
        const std::scoped_lock lock(mutex_);
        return status_;
    }
    /// Steady-clock age of the last publish; nullopt before the first one.
    [[nodiscard]] std::optional<std::chrono::steady_clock::duration> age() const {
        const std::scoped_lock lock(mutex_);
        if (!last_publish_) {
            return std::nullopt;
        }
        return std::chrono::steady_clock::now() - *last_publish_;
    }

  private:
    mutable std::mutex mutex_;
    ClusterStatus status_;
    std::optional<std::chrono::steady_clock::time_point> last_publish_;
};

}  // namespace faulttwin::api
