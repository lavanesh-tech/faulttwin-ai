#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "faulttwin/core/logger.hpp"
#include "faulttwin/observability/metrics.hpp"
#include "faulttwin/storage/incident_store.hpp"

namespace faulttwin::storage {

struct RecorderCounters {
    std::uint64_t opened{0};
    std::uint64_t escalated{0};
    std::uint64_t resolved{0};
    std::uint64_t orphans_closed{0};  ///< incidents closed as monitor_restarted at start
    std::uint64_t retries{0};         ///< flushes that stopped on an unavailable store
    std::uint64_t dropped{0};         ///< detections refused because the retry queue was full
    std::uint64_t rejected{0};        ///< permanent store errors (conflict, invalid, capacity)
    std::uint64_t unmatched{0};       ///< escalate/resolve with no known open incident
};

/// Turns the rule engine's detections into incident-store operations.
///
/// Detections are queued in arrival order and applied in that order, so an escalation can
/// never overtake the open it belongs to. When the store is unavailable the queue keeps the
/// rest and the next flush() retries from the same place; the queue is bounded (max_pending),
/// and a detection that does not fit is dropped, counted and logged at error level, so the
/// monitor loop never blocks or grows without bound on a dead meta-db. Permanent errors are
/// counted, logged and skipped. Single-threaded: owned by the monitor loop.
class IncidentRecorder {
  public:
    IncidentRecorder(IncidentStore& store, core::Logger& logger, std::string feature_schema_version,
                     std::size_t max_pending = 1024, observability::Registry* metrics = nullptr);

    /// Closes incidents left open by a previous process (Resolution::kMonitorRestarted). If
    /// the store is unavailable it is retried by later flushes before any detection.
    void start(Timestamp now);
    /// Queues `detections` and flushes.
    void record(const std::vector<detection::Detection>& detections);
    /// Applies queued operations until the queue is empty or the store is unavailable.
    void flush();

    [[nodiscard]] std::size_t pending() const noexcept { return queue_.size(); }
    [[nodiscard]] bool started() const noexcept { return started_; }
    [[nodiscard]] const RecorderCounters& counters() const noexcept { return counters_; }

  private:
    enum class Outcome : std::uint8_t { kApplied, kSkipped, kRetryLater };
    Outcome apply(const detection::Detection& d);
    Outcome on_error(const detection::Detection& d, const StoreError& error);
    void update_gauges();

    IncidentStore* store_;  ///< not owned; outlives the recorder
    core::Logger* logger_;  ///< not owned; outlives the recorder
    std::string feature_schema_version_;
    std::size_t max_pending_;
    std::deque<detection::Detection> queue_;
    std::map<std::pair<std::string, std::string>, std::uint64_t> open_ids_;
    bool started_{false};
    Timestamp start_time_;
    RecorderCounters counters_;

    struct Metrics {
        observability::Counter* opened{nullptr};
        observability::Counter* escalated{nullptr};
        observability::Counter* resolved{nullptr};
        observability::Counter* dropped{nullptr};
        observability::Counter* rejected{nullptr};
        observability::Counter* retries{nullptr};
        observability::Gauge* pending{nullptr};
    } metrics_;
};

}  // namespace faulttwin::storage
