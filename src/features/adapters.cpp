#include "faulttwin/features/adapters.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace faulttwin::features {

namespace col = faulttwin::collectors;

Observation observation_from_reading(std::string node, MetricId metric, Timestamp at,
                                     const col::Reading<double>& reading) {
    if (const auto& value = reading.value(); reading.is_ok() && value.has_value()) {
        return Observation::ok(std::move(node), metric, at, *value);
    }
    // kOk without a value cannot happen (Reading's invariant); map it to missing defensively.
    const Quality quality = reading.is_ok() ? Quality::kMissing : reading.quality();
    return Observation::missing(std::move(node), metric, at, quality);
}

namespace {

const col::PgNodeSnapshot* reachable_primary(std::span<const col::PgNodeSnapshot> nodes) {
    const col::PgNodeSnapshot* found = nullptr;
    for (const auto& n : nodes) {
        if (n.availability != col::Availability::kDown && n.role == telemetry::NodeRole::kPrimary) {
            if (found != nullptr) {
                return nullptr;  // two primaries: no trustworthy lag source
            }
            found = &n;
        }
    }
    return found;
}

void append_lag(std::vector<Observation>& out, const col::PgNodeSnapshot& node, Timestamp at,
                const col::PgNodeSnapshot* primary) {
    const std::string& name = node.node;
    if (node.role == telemetry::NodeRole::kPrimary) {
        out.push_back(Observation::missing(name, MetricId::kReplicationLagSeconds, at,
                                           Quality::kNotApplicable));
        out.push_back(Observation::missing(name, MetricId::kReplicationLagBytes, at,
                                           Quality::kNotApplicable));
        return;
    }
    const col::StandbyLink* link = nullptr;
    if (primary != nullptr) {
        const auto it = std::find_if(
            primary->standbys.begin(), primary->standbys.end(),
            [&](const col::StandbyLink& s) { return s.application_name == node.identity(); });
        if (it != primary->standbys.end()) {
            link = &*it;
        }
    }
    if (link == nullptr || node.availability == col::Availability::kDown) {
        out.push_back(Observation::missing(name, MetricId::kReplicationLagSeconds, at));
        out.push_back(Observation::missing(name, MetricId::kReplicationLagBytes, at));
        return;
    }
    out.push_back(
        observation_from_reading(name, MetricId::kReplicationLagBytes, at, link->replay_lag_bytes));
    const bool caught_up = link->replay_lag_bytes.is_ok() && link->replay_lag_bytes.value() &&
                           *link->replay_lag_bytes.value() == 0.0;
    if (link->replay_lag_seconds.quality() == Quality::kMissing && caught_up) {
        out.push_back(Observation::ok(name, MetricId::kReplicationLagSeconds, at, 0.0));
    } else {
        out.push_back(observation_from_reading(name, MetricId::kReplicationLagSeconds, at,
                                               link->replay_lag_seconds));
    }
}

}  // namespace

std::vector<Observation> observations_from_cycle(
    std::span<const col::PgNodeSnapshot> nodes,
    std::span<const std::optional<col::PgRates>> rates) {
    std::vector<Observation> out;
    out.reserve(nodes.size() * 7);
    const col::PgNodeSnapshot* primary = reachable_primary(nodes);
    const bool rates_aligned = rates.size() == nodes.size();

    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        const bool down = node.availability == col::Availability::kDown;
        const Timestamp at = down ? node.attempted_at : node.collected_at;
        const std::string& name = node.node;

        if (down) {
            out.push_back(Observation::missing(name, MetricId::kAvailability, at));
        } else {
            out.push_back(Observation::ok(name, MetricId::kAvailability, at, 1.0));
        }
        out.push_back(
            observation_from_reading(name, MetricId::kProbeLatencyMs, at, node.probe_latency_ms));
        out.push_back(observation_from_reading(name, MetricId::kConnectionUtilization, at,
                                               node.connections.utilization_percent));

        const std::optional<col::PgRates>* node_rates = rates_aligned ? &rates[i] : nullptr;
        if (node_rates != nullptr && node_rates->has_value()) {
            const auto& r = **node_rates;
            out.push_back(observation_from_reading(name, MetricId::kCommitsPerSecond, at,
                                                   r.commits_per_second));
            out.push_back(observation_from_reading(name, MetricId::kStatementLatencyMs, at,
                                                   r.mean_statement_latency_ms));
        } else {
            out.push_back(Observation::missing(name, MetricId::kCommitsPerSecond, at));
            out.push_back(Observation::missing(name, MetricId::kStatementLatencyMs, at));
        }
        if (down) {
            out.push_back(Observation::missing(name, MetricId::kReplicationLagSeconds, at));
            out.push_back(Observation::missing(name, MetricId::kReplicationLagBytes, at));
        } else {
            append_lag(out, node, at, primary);
        }
    }
    return out;
}

std::vector<Observation> observations_from_sample(const telemetry::TelemetrySample& sample) {
    const std::string node{sample.node_id().value()};
    const Timestamp at = sample.collected_at();
    std::vector<Observation> out;
    out.reserve(6);
    out.push_back(Observation::ok(node, MetricId::kAvailability, at, 1.0));
    out.push_back(Observation::ok(node, MetricId::kCpuPercent, at, sample.cpu_usage_percent()));
    out.push_back(
        Observation::ok(node, MetricId::kMemoryPercent, at, sample.memory_usage_percent()));
    out.push_back(Observation::ok(node, MetricId::kDiskPercent, at, sample.disk_usage_percent()));
    switch (sample.replication_lag_state()) {
        case telemetry::ReplicationLagState::kMeasured:
            out.push_back(Observation::ok(node, MetricId::kReplicationLagSeconds, at,
                                          sample.replication_lag_ms().value_or(0.0) / 1000.0));
            break;
        case telemetry::ReplicationLagState::kNotMeasured:
            out.push_back(Observation::missing(node, MetricId::kReplicationLagSeconds, at));
            break;
        case telemetry::ReplicationLagState::kNotApplicable:
            out.push_back(Observation::missing(node, MetricId::kReplicationLagSeconds, at,
                                               Quality::kNotApplicable));
            break;
    }
    return out;
}

}  // namespace faulttwin::features
