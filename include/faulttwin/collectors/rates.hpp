#pragma once

#include "faulttwin/collectors/pg_snapshot.hpp"
#include "faulttwin/collectors/reading.hpp"

namespace faulttwin::collectors {

/// Per-second rates derived from two snapshots of the same node.
struct PgRates {
    Reading<double> interval_seconds;
    Reading<double> commits_per_second;  ///< transaction throughput
    Reading<double> rollbacks_per_second;
    Reading<double> wal_bytes_per_second;  ///< primary only
    /// Mean server execution time per statement over the interval (pg_stat_statements).
    /// kMissing when no statement completed in the interval, rather than 0 ms.
    Reading<double> mean_statement_latency_ms;
};

/// Rates over the steady-clock interval between `previous` and `current`. A counter that went
/// backwards (stats reset, restart, failover) yields kInvalid for that interval, never a
/// negative rate. Snapshots of different nodes yield kInvalid everywhere.
[[nodiscard]] PgRates compute_rates(const PgNodeSnapshot& previous, const PgNodeSnapshot& current);

}  // namespace faulttwin::collectors
