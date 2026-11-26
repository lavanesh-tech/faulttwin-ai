#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "faulttwin/collectors/pg_snapshot.hpp"
#include "faulttwin/collectors/reading.hpp"

namespace faulttwin::collectors {

enum class TopologyIssueCode : std::uint8_t {
    kNodeDown,
    kRoleUnknown,  ///< reachable but the role could not be read
    kNoReachablePrimary,
    kMultiplePrimaries,        ///< split-brain risk
    kReplicaNotStreaming,      ///< replica's WAL receiver is not in state "streaming"
    kReplicaNotSeenByPrimary,  ///< no pg_stat_replication row for this replica
    kUnknownStandby,           ///< primary streams to a standby that is not in the inventory
    kSlotInactive,             ///< a physical slot has no consumer (WAL is being retained)
};
[[nodiscard]] std::string_view to_string(TopologyIssueCode code) noexcept;

struct TopologyIssue {
    TopologyIssueCode code;
    std::string node;
    std::string detail;
};

/// A replication edge as observed from the primary (pg_stat_replication).
struct ReplicationEdge {
    std::string upstream;
    std::string downstream;
    std::string state;
    Reading<double> replay_lag_bytes;
    Reading<double> replay_lag_seconds;
};

struct TopologySnapshot {
    std::optional<std::string> primary;  ///< set only when exactly one primary is reachable
    std::vector<std::string> replicas;
    std::vector<ReplicationEdge> edges;
    std::vector<TopologyIssue> issues;
    [[nodiscard]] bool healthy() const noexcept { return issues.empty(); }
};

/// Derives roles and edges from one collection cycle. Pure; issue order is deterministic
/// (node issues in input order, then primary-side issues).
[[nodiscard]] TopologySnapshot derive_topology(std::span<const PgNodeSnapshot> nodes);

}  // namespace faulttwin::collectors
