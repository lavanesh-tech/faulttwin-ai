#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "faulttwin/api/status_board.hpp"
#include "faulttwin/collectors/libpq_session.hpp"
#include "faulttwin/core/logger.hpp"
#include "faulttwin/detection/online_detector.hpp"
#include "faulttwin/detection/scenario.hpp"
#include "faulttwin/observability/metrics.hpp"
#include "faulttwin/storage/incident_recorder.hpp"

namespace faulttwin::server {

/// What one monitoring cycle produced.
struct CycleOutput {
    std::vector<detection::Detection> detections;
    std::vector<api::NodeStatus> nodes;
    std::vector<api::FiringAlert> firing;
};

/// Where telemetry comes from. One implementation per source; called from the monitor thread
/// only, so implementations need not be thread-safe.
class CycleSource {
  public:
    CycleSource() = default;
    CycleSource(const CycleSource&) = delete;
    CycleSource& operator=(const CycleSource&) = delete;
    CycleSource(CycleSource&&) = delete;
    CycleSource& operator=(CycleSource&&) = delete;
    virtual ~CycleSource() = default;

    [[nodiscard]] virtual CycleOutput next() = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
};

/// Node status derived from one cycle's observations: availability from the availability
/// metric (ok = up, anything else = down), lag and probe latency when they were measured.
[[nodiscard]] std::vector<api::NodeStatus> node_status_from_observations(
    const std::vector<features::Observation>& observations, core::WallTime at);

/// The live cluster through the Phase 3 collectors (read-only role), then Phase 4 features
/// and rules.
class PgCycleSource final : public CycleSource {
  public:
    PgCycleSource(const std::vector<collectors::PgEndpoint>& nodes,
                  const collectors::PgCredentials& credentials,
                  const collectors::ConnectOptions& connect, std::chrono::milliseconds max_age);
    [[nodiscard]] CycleOutput next() override;
    [[nodiscard]] std::string_view name() const noexcept override { return "postgres"; }

  private:
    std::vector<collectors::PgNodeCollector> collectors_;
    std::vector<std::optional<collectors::PgNodeSnapshot>> previous_;
    std::chrono::milliseconds max_age_;
    detection::OnlineDetector detector_;
};

/// A Phase 4 synthetic scenario replayed one tick per cycle, for demos and for end-to-end
/// tests without Docker. Its timestamps are the scenario's own event times (2026-01-01 by
/// default), not the wall clock, and every value is synthetic. After the last tick the source
/// keeps reporting the final node status without new observations.
class SyntheticCycleSource final : public CycleSource {
  public:
    explicit SyntheticCycleSource(std::uint64_t seed);
    [[nodiscard]] CycleOutput next() override;
    [[nodiscard]] std::string_view name() const noexcept override { return "synthetic"; }
    [[nodiscard]] bool finished() const noexcept {
        return cursor_ >= scenario_.observations.size();
    }

  private:
    detection::Scenario scenario_;
    std::size_t cursor_{0};
    detection::OnlineDetector detector_;
    std::vector<api::NodeStatus> last_nodes_;
};

/// Runs the source every `interval` on its own std::jthread: detections go to the incident
/// recorder, status to the board, counters to the registry. stop() (or destruction) requests
/// stop, wakes the sleeping thread at once (condition_variable_any with the stop token, no
/// polling) and joins; the cycle in progress finishes first, then the recorder gets one last
/// flush so queued detections are not lost if the store is reachable.
class MonitorLoop {
  public:
    MonitorLoop(std::unique_ptr<CycleSource> source, storage::IncidentRecorder& recorder,
                api::StatusBoard& board, core::Logger& logger, observability::Registry& metrics,
                std::chrono::milliseconds interval);
    MonitorLoop(const MonitorLoop&) = delete;
    MonitorLoop& operator=(const MonitorLoop&) = delete;
    MonitorLoop(MonitorLoop&&) = delete;
    MonitorLoop& operator=(MonitorLoop&&) = delete;
    ~MonitorLoop();

    void start();
    void stop();

  private:
    void run(const std::stop_token& stop);
    void run_cycle();

    std::unique_ptr<CycleSource> source_;
    storage::IncidentRecorder& recorder_;
    api::StatusBoard& board_;
    core::Logger& logger_;
    std::chrono::milliseconds interval_;
    std::uint64_t cycles_{0};
    observability::Counter& cycles_total_;
    observability::Counter& cycle_errors_total_;
    observability::Histogram& cycle_seconds_;
    observability::Registry& metrics_;
    std::mutex sleep_mutex_;
    std::condition_variable_any sleep_cv_;
    std::jthread thread_;  ///< last member: joined before the members it uses are destroyed
};

}  // namespace faulttwin::server
