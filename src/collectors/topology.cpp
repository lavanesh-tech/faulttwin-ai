#include "faulttwin/collectors/topology.hpp"

#include <algorithm>
#include <string>

namespace faulttwin::collectors {

std::string_view to_string(TopologyIssueCode code) noexcept {
    switch (code) {
        case TopologyIssueCode::kNodeDown:
            return "node_down";
        case TopologyIssueCode::kRoleUnknown:
            return "role_unknown";
        case TopologyIssueCode::kNoReachablePrimary:
            return "no_reachable_primary";
        case TopologyIssueCode::kMultiplePrimaries:
            return "multiple_primaries";
        case TopologyIssueCode::kReplicaNotStreaming:
            return "replica_not_streaming";
        case TopologyIssueCode::kReplicaNotSeenByPrimary:
            return "replica_not_seen_by_primary";
        case TopologyIssueCode::kUnknownStandby:
            return "unknown_standby";
        case TopologyIssueCode::kSlotInactive:
            return "slot_inactive";
    }
    return "unknown";
}

namespace {

/// Node-level issues; returns the reachable primaries.
std::vector<const PgNodeSnapshot*> classify_nodes(std::span<const PgNodeSnapshot> nodes,
                                                  TopologySnapshot& topology) {
    std::vector<const PgNodeSnapshot*> primaries;
    for (const auto& node : nodes) {
        if (node.availability == Availability::kDown) {
            const std::string detail = node.errors.empty()
                                           ? std::string{}
                                           : std::string(to_string(node.errors.front().kind));
            topology.issues.push_back({TopologyIssueCode::kNodeDown, node.node, detail});
            continue;
        }
        if (node.role == NodeRole::kPrimary) {
            primaries.push_back(&node);
        } else if (node.role == NodeRole::kReplica) {
            topology.replicas.push_back(node.node);
            const auto& status = node.replica.receiver_status;
            if (!status.is_ok() || status.measured() != "streaming") {
                topology.issues.push_back({TopologyIssueCode::kReplicaNotStreaming, node.node,
                                           status.is_ok() ? status.measured() : status.detail()});
            }
        } else {
            topology.issues.push_back({TopologyIssueCode::kRoleUnknown, node.node, {}});
        }
    }
    return primaries;
}

/// Edges and issues from the single primary's pg_stat_replication and slot views.
void check_primary_view(const PgNodeSnapshot& primary, std::span<const PgNodeSnapshot> nodes,
                        TopologySnapshot& topology) {
    for (const auto& link : primary.standbys) {
        const auto match = std::ranges::find_if(nodes, [&](const PgNodeSnapshot& n) {
            return n.identity() == link.application_name || n.node == link.application_name;
        });
        if (match == nodes.end()) {
            topology.issues.push_back(
                {TopologyIssueCode::kUnknownStandby, link.application_name, link.client_addr});
            continue;
        }
        topology.edges.push_back(ReplicationEdge{primary.node, match->node, link.state,
                                                 link.replay_lag_bytes, link.replay_lag_seconds});
    }
    // Replicas that are up but absent from the primary's pg_stat_replication. Only meaningful
    // when the primary's standby list was actually read.
    if (primary.standby_count.is_ok()) {
        for (const auto& replica : topology.replicas) {
            const bool seen = std::ranges::any_of(
                topology.edges, [&](const ReplicationEdge& e) { return e.downstream == replica; });
            if (!seen) {
                topology.issues.push_back(
                    {TopologyIssueCode::kReplicaNotSeenByPrimary, replica, {}});
            }
        }
    }
    for (const auto& slot : primary.slots) {
        if (!slot.active) {
            topology.issues.push_back(
                {TopologyIssueCode::kSlotInactive, slot.name, slot.wal_status});
        }
    }
}

}  // namespace

TopologySnapshot derive_topology(std::span<const PgNodeSnapshot> nodes) {
    TopologySnapshot topology;
    const auto primaries = classify_nodes(nodes, topology);
    if (primaries.empty()) {
        topology.issues.push_back({TopologyIssueCode::kNoReachablePrimary, {}, {}});
        return topology;
    }
    if (primaries.size() > 1) {
        std::string names;
        for (const auto* p : primaries) {
            names += (names.empty() ? "" : ",") + p->node;
        }
        topology.issues.push_back({TopologyIssueCode::kMultiplePrimaries, {}, names});
        return topology;
    }
    topology.primary = primaries.front()->node;
    check_primary_view(*primaries.front(), nodes, topology);
    return topology;
}

}  // namespace faulttwin::collectors
