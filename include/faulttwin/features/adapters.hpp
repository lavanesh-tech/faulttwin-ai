#pragma once

#include <optional>
#include <span>
#include <vector>

#include "faulttwin/collectors/pg_snapshot.hpp"
#include "faulttwin/collectors/rates.hpp"
#include "faulttwin/features/observation.hpp"
#include "faulttwin/telemetry/telemetry_sample.hpp"

namespace faulttwin::features {

/// Observations from one collection cycle of the Phase 3 monitor. `rates[i]` belongs to
/// `nodes[i]` (nullopt when there is no previous snapshot or either one is down); the spans
/// must have the same length, otherwise rates are treated as absent.
///
/// Mapping (ADR-0016):
/// - event time = collected_at for reachable nodes, attempted_at for down nodes;
/// - availability = ok 1.0 when the node is up or degraded, kMissing when down (never 0);
/// - replication lag of a replica comes from the primary's pg_stat_replication row for it.
///   A NULL replay_lag on a caught-up standby (replay_lag_bytes == 0) becomes ok 0 s, because
///   PostgreSQL clears the column when there is nothing to replay. On the primary, lag is
///   kNotApplicable. With no reachable primary, or no row for the replica, lag is kMissing;
/// - every other metric keeps the quality of the Reading it came from.
/// Host CPU, memory and disk are not per node in Phase 3 (they describe the collector's host),
/// so they are not mapped here.
[[nodiscard]] std::vector<Observation> observations_from_cycle(
    std::span<const collectors::PgNodeSnapshot> nodes,
    std::span<const std::optional<collectors::PgRates>> rates);

/// Observations from a validated Phase 1 sample (synthetic telemetry): CPU, memory, disk,
/// availability 1.0, and replication lag in seconds when it was measured (kMissing when not
/// measured, kNotApplicable on a primary).
[[nodiscard]] std::vector<Observation> observations_from_sample(
    const telemetry::TelemetrySample& sample);

/// Maps a collector reading to an observation of `metric`.
[[nodiscard]] Observation observation_from_reading(std::string node, MetricId metric, Timestamp at,
                                                   const collectors::Reading<double>& reading);

}  // namespace faulttwin::features
