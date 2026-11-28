#include "monitor_loop.hpp"

#include <algorithm>
#include <map>

#include "faulttwin/collectors/rates.hpp"
#include "faulttwin/features/adapters.hpp"

namespace faulttwin::server {

std::vector<api::NodeStatus> node_status_from_observations(
    const std::vector<features::Observation>& observations, core::WallTime at) {
    std::map<std::string, api::NodeStatus> by_node;
    for (const auto& o : observations) {
        auto& status = by_node[o.node];
        status.node = o.node;
        status.observed_at = std::max(status.observed_at, o.at);
        if (status.availability.empty()) {
            status.availability = "down";
            status.role = o.node.find("primary") != std::string::npos ? "primary" : "replica";
        }
        const bool ok = o.quality == collectors::Quality::kOk;
        switch (o.metric) {
            case features::MetricId::kAvailability:
                status.availability = ok && o.value >= 1.0 ? "up" : "down";
                break;
            case features::MetricId::kReplicationLagSeconds:
                if (ok) {
                    status.replication_lag_seconds = o.value;
                }
                break;
            case features::MetricId::kProbeLatencyMs:
                if (ok) {
                    status.probe_latency_ms = o.value;
                }
                break;
            default:
                break;
        }
    }
    std::vector<api::NodeStatus> out;
    out.reserve(by_node.size());
    for (auto& [name, status] : by_node) {
        if (status.observed_at == core::WallTime{}) {
            status.observed_at = at;
        }
        out.push_back(std::move(status));
    }
    return out;
}

namespace {
std::vector<api::FiringAlert> firing_alerts(const detection::OnlineDetector& detector) {
    std::vector<api::FiringAlert> out;
    for (auto& [rule, node] : detector.firing()) {
        out.push_back({rule, node});
    }
    return out;
}
}  // namespace

PgCycleSource::PgCycleSource(const std::vector<collectors::PgEndpoint>& nodes,
                             const collectors::PgCredentials& credentials,
                             const collectors::ConnectOptions& connect,
                             std::chrono::milliseconds max_age)
    : previous_(nodes.size()), max_age_(max_age) {
    collectors_.reserve(nodes.size());
    for (const auto& endpoint : nodes) {
        collectors_.emplace_back(endpoint, credentials, connect);
    }
}

CycleOutput PgCycleSource::next() {
    std::vector<collectors::PgNodeSnapshot> snapshots;
    std::vector<std::optional<collectors::PgRates>> rates;
    snapshots.reserve(collectors_.size());
    for (auto& collector : collectors_) {
        snapshots.push_back(collector.collect());
    }
    for (std::size_t i = 0; i < snapshots.size(); ++i) {
        const auto& prev = previous_[i];
        const auto& curr = snapshots[i];
        const bool both = prev && prev->availability != collectors::Availability::kDown &&
                          curr.availability != collectors::Availability::kDown;
        rates.push_back(both ? std::optional{collectors::compute_rates(*prev, curr)}
                             : std::nullopt);
    }
    const auto now = core::WallClock::now();
    for (std::size_t i = 0; i < snapshots.size(); ++i) {
        previous_[i] = snapshots[i];
        (void)collectors::mark_stale_if_older(snapshots[i], now, max_age_);
    }
    CycleOutput out;
    out.detections = detector_.on_cycle(snapshots, rates);
    out.nodes =
        node_status_from_observations(features::observations_from_cycle(snapshots, rates), now);
    // The collector knows each node's real role and its availability class (up / degraded /
    // down); prefer those over what the observations imply.
    for (auto& status : out.nodes) {
        for (const auto& s : snapshots) {
            if (s.node == status.node) {
                status.role = std::string(telemetry::to_string(s.role));
                status.availability = std::string(collectors::to_string(s.availability));
            }
        }
    }
    out.firing = firing_alerts(detector_);
    return out;
}

SyntheticCycleSource::SyntheticCycleSource(std::uint64_t seed) {
    detection::ScenarioConfig config;
    config.seed = seed;
    scenario_ = detection::generate_scenario(config);
}

CycleOutput SyntheticCycleSource::next() {
    CycleOutput out;
    if (finished()) {
        out.nodes = last_nodes_;
        out.firing = firing_alerts(detector_);
        return out;
    }
    // One scenario tick: every observation sharing the next event time.
    const auto tick = scenario_.observations[cursor_].at;
    std::vector<features::Observation> batch;
    while (cursor_ < scenario_.observations.size() && scenario_.observations[cursor_].at == tick) {
        batch.push_back(scenario_.observations[cursor_]);
        ++cursor_;
    }
    out.detections = detector_.on_observations(batch, tick);
    auto nodes = node_status_from_observations(batch, tick);
    // A node with no observation this tick (a telemetry gap) keeps its previous entry, marked
    // down: silence is not health.
    for (const auto& previous : last_nodes_) {
        const bool present = std::ranges::any_of(
            nodes, [&](const api::NodeStatus& n) { return n.node == previous.node; });
        if (!present) {
            auto gap = previous;
            gap.availability = "down";
            nodes.push_back(std::move(gap));
        }
    }
    std::ranges::sort(nodes, {}, &api::NodeStatus::node);
    last_nodes_ = nodes;
    out.nodes = std::move(nodes);
    out.firing = firing_alerts(detector_);
    return out;
}

MonitorLoop::MonitorLoop(std::unique_ptr<CycleSource> source, storage::IncidentRecorder& recorder,
                         api::StatusBoard& board, core::Logger& logger,
                         observability::Registry& metrics, std::chrono::milliseconds interval)
    : source_(std::move(source)),
      recorder_(recorder),
      board_(board),
      logger_(logger),
      interval_(interval),
      cycles_total_(metrics.counter("faulttwin_monitor_cycles_total", "Completed monitor cycles")),
      cycle_errors_total_(metrics.counter("faulttwin_monitor_cycle_errors_total",
                                          "Monitor cycles that threw an exception")),
      cycle_seconds_(metrics.histogram("faulttwin_monitor_cycle_duration_seconds",
                                       "Wall time of one monitor cycle (collect, detect, record)",
                                       observability::default_latency_buckets())),
      metrics_(metrics) {}

MonitorLoop::~MonitorLoop() {
    stop();
}

void MonitorLoop::start() {
    thread_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
}

void MonitorLoop::stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

void MonitorLoop::run_cycle() {
    const auto started = std::chrono::steady_clock::now();
    CycleOutput out = source_->next();
    for (const auto& d : out.detections) {
        metrics_
            .counter("faulttwin_detections_total", "Rule state changes by kind",
                     {"kind", "severity"},
                     {std::string(detection::to_string(d.kind)),
                      std::string(detection::to_string(d.severity))})
            .inc();
    }
    recorder_.record(out.detections);
    for (const auto& n : out.nodes) {
        metrics_
            .gauge("faulttwin_node_up", "1 if the node was reachable in the last cycle", {"node"},
                   {n.node})
            .set(n.availability == "down" ? 0.0 : 1.0);
    }
    ++cycles_;
    api::ClusterStatus status;
    status.cycles = cycles_;
    status.last_cycle_at = core::WallClock::now();
    status.source = std::string(source_->name());
    status.nodes = std::move(out.nodes);
    status.firing = std::move(out.firing);
    board_.publish(std::move(status));
    cycles_total_.inc();
    cycle_seconds_.observe(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
}

void MonitorLoop::run(const std::stop_token& stop) {
    logger_.info("monitor", "monitor loop started",
                 {{"source", std::string(source_->name())},
                  {"interval_ms", static_cast<std::int64_t>(interval_.count())}});
    auto next = std::chrono::steady_clock::now();
    while (!stop.stop_requested()) {
        try {
            run_cycle();
        } catch (const std::exception& e) {
            cycle_errors_total_.inc();
            logger_.error("monitor", "monitor cycle failed", {{"detail", e.what()}});
        }
        next += interval_;
        // Skip cycles we are too late for instead of bursting to catch up.
        const auto now = std::chrono::steady_clock::now();
        if (next < now) {
            next = now;
        }
        std::unique_lock lock(sleep_mutex_);
        (void)sleep_cv_.wait_until(lock, stop, next, [] { return false; });
    }
    recorder_.flush();
    logger_.info("monitor", "monitor loop stopped",
                 {{"cycles", cycles_},
                  {"pending_incident_writes", static_cast<std::uint64_t>(recorder_.pending())}});
}

}  // namespace faulttwin::server
