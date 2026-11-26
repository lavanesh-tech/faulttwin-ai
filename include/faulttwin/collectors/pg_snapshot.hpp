#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "faulttwin/collectors/lsn.hpp"
#include "faulttwin/collectors/reading.hpp"
#include "faulttwin/collectors/sql.hpp"
#include "faulttwin/telemetry/node_role.hpp"
#include "faulttwin/telemetry/telemetry_sample.hpp"

namespace faulttwin::collectors {

using telemetry::NodeRole;
using telemetry::Timestamp;
using SteadyTime = std::chrono::steady_clock::time_point;

/// Injectable clocks so that collection logic is deterministic in tests.
struct CollectorClock {
    std::function<Timestamp()> wall;
    std::function<SteadyTime()> steady;
    [[nodiscard]] static CollectorClock system();
};

/// kUp: connected and every query succeeded. kDegraded: connected, but at least one query
/// failed (its metrics are kMissing with the reason). kDown: no connection; nothing measured.
enum class Availability : std::uint8_t { kUp, kDegraded, kDown };
[[nodiscard]] std::string_view to_string(Availability availability) noexcept;

struct ConnectionStats {
    Reading<double> client_backends;
    Reading<double> active;
    Reading<double> idle_in_transaction;
    Reading<double> waiting_on_lock;
    Reading<double> max_connections;
    Reading<double> utilization_percent;  ///< client_backends / max_connections × 100
};

/// One row of pg_stat_replication: the primary's view of one connected standby.
struct StandbyLink {
    std::string application_name;
    std::string client_addr;
    std::string state;
    std::string sync_state;
    Reading<Lsn> sent_lsn;
    Reading<Lsn> flush_lsn;
    Reading<Lsn> replay_lsn;
    Reading<double> replay_lag_bytes;  ///< primary's current LSN - standby replay LSN
    /// PostgreSQL resets these to NULL when the standby has caught up and the primary is idle,
    /// so NULL is reported as kMissing with that reason, never as 0.
    Reading<double> write_lag_seconds;
    Reading<double> flush_lag_seconds;
    Reading<double> replay_lag_seconds;
};

struct ReplicationSlot {
    std::string name;
    bool active{false};
    std::string wal_status;
    Reading<double> retained_bytes;  ///< primary's current LSN - restart_lsn
};

/// The replica's own view of its recovery.
struct ReplicaState {
    Reading<std::string> receiver_status;  ///< pg_stat_wal_receiver.status ("streaming", ...)
    std::string sender_host;
    Reading<Lsn> receive_lsn;
    Reading<Lsn> replay_lsn;
    Reading<double> apply_backlog_bytes;  ///< received but not yet replayed
    /// 0 when the receiver is streaming and everything received has been replayed; otherwise
    /// kMissing. A time-based lag while WAL is pending cannot be derived on the replica (see
    /// last_replay_age_seconds); the primary's pg_stat_replication.replay_lag is the source.
    Reading<double> replay_lag_seconds;
    /// now() - pg_last_xact_replay_timestamp(): age of the last replayed *commit*. This is not
    /// lag: it grows without bound on an idle primary, and WAL without commits (checkpoints,
    /// standby snapshots) does not reset it.
    Reading<double> last_replay_age_seconds;
    Reading<double> last_message_age_seconds;  ///< now - last message from the primary
    Reading<bool> replay_paused;
};

/// Everything collected from one node in one cycle.
struct PgNodeSnapshot {
    std::string node;          ///< inventory name
    std::string cluster_name;  ///< reported by the server; empty if unknown
    Timestamp attempted_at;
    Timestamp collected_at;
    SteadyTime collected_steady;
    Availability availability{Availability::kDown};
    std::vector<SqlError> errors;  ///< connection error or every failed query, in order
    NodeRole role{NodeRole::kUnknown};

    Reading<double> connect_latency_ms;  ///< time to (re)connect; n/a when reusing a connection
    Reading<double> probe_latency_ms;    ///< client-side round trip of SELECT 1
    Reading<double> server_version_num;
    ConnectionStats connections;
    Reading<double> xact_commit_total;    ///< cumulative, all databases
    Reading<double> xact_rollback_total;  ///< cumulative, all databases
    /// pg_stat_statements totals excluding the monitoring role's own statements.
    Reading<double> statements_calls_total;
    Reading<double> statements_exec_time_ms_total;
    Reading<Lsn> current_wal_lsn;  ///< primary only

    Reading<double> standby_count;  ///< primary only; quality also covers `standbys`
    std::vector<StandbyLink> standbys;
    Reading<double> slot_count;  ///< primary only; quality also covers `slots`
    std::vector<ReplicationSlot> slots;
    ReplicaState replica;  ///< replica only; every reading is kNotApplicable on a primary

    /// Transactions the collector itself ended on this node during this collection. All
    /// queries of one collection run in a single read-only transaction, so this is normally
    /// one commit; compute_rates subtracts it so the observer does not inflate throughput.
    std::uint32_t own_commits{0};
    std::uint32_t own_rollbacks{0};

    /// The name used to match this node to replication edges.
    [[nodiscard]] const std::string& identity() const noexcept {
        return cluster_name.empty() ? node : cluster_name;
    }
};

struct CollectionPolicy {
    /// A replica whose WAL receiver has heard nothing from the primary for longer than this has
    /// its receive-derived readings (apply backlog, replay lag) marked kStale: they describe
    /// what it received, which may no longer be what the primary has.
    std::chrono::milliseconds max_receiver_silence{std::chrono::seconds{30}};
};

/// Collects one snapshot through an already connected session. Never throws for bad server
/// data: every failure becomes a reading quality plus an entry in `errors`.
[[nodiscard]] PgNodeSnapshot collect_pg_node(
    SqlSession& session, std::string node, const CollectorClock& clock,
    const CollectionPolicy& policy = {},
    Reading<double> connect_latency_ms = Reading<double>::not_applicable("reused"));

/// Snapshot for a node that could not be reached: availability kDown, role unknown, every
/// metric kMissing with the connection error as the reason.
[[nodiscard]] PgNodeSnapshot unreachable_snapshot(std::string node, Timestamp attempted_at,
                                                  const SqlError& error,
                                                  const CollectorClock& clock);

/// Calls `visit(name, reading)` for every scalar reading of the snapshot (LSNs, booleans and
/// strings included). Used for staleness marking, coverage counting and reporting.
template <typename Snapshot, typename Visitor>
void for_each_reading(Snapshot& s, const Visitor& visit);

/// Marks every kOk reading kStale when the snapshot is older than `max_age` at `now`.
/// Returns true if the snapshot was stale.
bool mark_stale_if_older(PgNodeSnapshot& snapshot, Timestamp now,
                         std::chrono::milliseconds max_age);

/// MON-COV for one node: kOk readings / readings that apply (everything except kNotApplicable).
struct Coverage {
    int ok{0};
    int applicable{0};
};
[[nodiscard]] Coverage coverage(const PgNodeSnapshot& snapshot);

// ---- implementation of the visitor template ------------------------------------------------

template <typename Snapshot, typename Visitor>
void for_each_reading(Snapshot& s, const Visitor& visit) {
    visit("connect_latency_ms", s.connect_latency_ms);
    visit("probe_latency_ms", s.probe_latency_ms);
    visit("server_version_num", s.server_version_num);
    visit("connections.client_backends", s.connections.client_backends);
    visit("connections.active", s.connections.active);
    visit("connections.idle_in_transaction", s.connections.idle_in_transaction);
    visit("connections.waiting_on_lock", s.connections.waiting_on_lock);
    visit("connections.max_connections", s.connections.max_connections);
    visit("connections.utilization_percent", s.connections.utilization_percent);
    visit("xact_commit_total", s.xact_commit_total);
    visit("xact_rollback_total", s.xact_rollback_total);
    visit("statements_calls_total", s.statements_calls_total);
    visit("statements_exec_time_ms_total", s.statements_exec_time_ms_total);
    visit("current_wal_lsn", s.current_wal_lsn);
    visit("standby_count", s.standby_count);
    visit("slot_count", s.slot_count);
    visit("replica.receiver_status", s.replica.receiver_status);
    visit("replica.receive_lsn", s.replica.receive_lsn);
    visit("replica.replay_lsn", s.replica.replay_lsn);
    visit("replica.apply_backlog_bytes", s.replica.apply_backlog_bytes);
    visit("replica.replay_lag_seconds", s.replica.replay_lag_seconds);
    visit("replica.last_replay_age_seconds", s.replica.last_replay_age_seconds);
    visit("replica.last_message_age_seconds", s.replica.last_message_age_seconds);
    visit("replica.replay_paused", s.replica.replay_paused);
}

}  // namespace faulttwin::collectors
