#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include "faulttwin/collectors/pg_snapshot.hpp"

namespace faulttwin::collectors {
namespace {

// Every statement is a single read-only SELECT over statistics views or monitoring functions.
// Booleans come back as "t"/"f" and intervals are converted to seconds on the server.
constexpr std::string_view kIdentitySql =
    "SELECT pg_is_in_recovery(), current_setting('server_version_num'), "
    "current_setting('cluster_name'), current_setting('max_connections')";
constexpr std::string_view kProbeSql = "SELECT 1";
constexpr std::string_view kConnectionsSql =
    "SELECT count(*), count(*) FILTER (WHERE state = 'active'), "
    "count(*) FILTER (WHERE state IN ('idle in transaction', 'idle in transaction (aborted)')), "
    "count(*) FILTER (WHERE wait_event_type = 'Lock') "
    "FROM pg_stat_activity WHERE backend_type = 'client backend'";
constexpr std::string_view kTransactionsSql =
    "SELECT sum(xact_commit), sum(xact_rollback) FROM pg_stat_database";
// The monitoring role's own statements are excluded so that collecting does not dilute the
// latency it measures.
constexpr std::string_view kStatementsPresentSql =
    "SELECT to_regclass('pg_stat_statements') IS NOT NULL";
constexpr std::string_view kStatementsSql =
    "SELECT coalesce(sum(calls), 0), coalesce(sum(total_exec_time), 0) FROM pg_stat_statements "
    "WHERE userid <> (SELECT oid FROM pg_roles WHERE rolname = current_user)";
constexpr std::string_view kForceStatsFlushSql = "SELECT pg_stat_force_next_flush()";
constexpr std::string_view kPrimaryWalSql = "SELECT pg_current_wal_lsn()";
constexpr std::string_view kStandbysSql =
    "SELECT application_name, coalesce(host(client_addr), ''), coalesce(state, ''), "
    "coalesce(sync_state, ''), sent_lsn, flush_lsn, replay_lsn, extract(epoch FROM write_lag), "
    "extract(epoch FROM flush_lag), extract(epoch FROM replay_lag), pg_current_wal_lsn() "
    "FROM pg_stat_replication ORDER BY application_name";
constexpr std::string_view kSlotsSql =
    "SELECT slot_name, active, coalesce(wal_status, ''), restart_lsn, pg_current_wal_lsn() "
    "FROM pg_replication_slots WHERE slot_type = 'physical' ORDER BY slot_name";
constexpr std::string_view kReplicaSql =
    "SELECT pg_last_wal_receive_lsn(), pg_last_wal_replay_lsn(), "
    "extract(epoch FROM now() - pg_last_xact_replay_timestamp()), pg_is_wal_replay_paused()";
constexpr std::string_view kReceiverSql =
    "SELECT status, coalesce(sender_host, ''), extract(epoch FROM now() - last_msg_receipt_time) "
    "FROM pg_stat_wal_receiver";

using Field = std::optional<std::string>;

double elapsed_ms(SteadyTime start, SteadyTime end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

std::optional<double> parse_finite(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(text.c_str(), &end);
    if (errno != 0 || end != text.c_str() + text.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

/// A numeric field. NULL is missing (not zero); garbage or a negative value is invalid.
Reading<double> number(const Field& field, std::string_view null_reason = "NULL from server") {
    if (!field) {
        return Reading<double>::missing(std::string(null_reason));
    }
    const auto value = parse_finite(*field);
    if (!value) {
        return Reading<double>::invalid("not a finite number: '" + sanitize_message(*field, 40) +
                                        "'");
    }
    if (*value < 0.0) {
        return Reading<double>::invalid("negative value: " + *field);
    }
    return Reading<double>::ok(*value);
}

Reading<Lsn> lsn(const Field& field, std::string_view null_reason = "NULL from server") {
    if (!field) {
        return Reading<Lsn>::missing(std::string(null_reason));
    }
    const auto parsed = parse_lsn(*field);
    if (!parsed) {
        return Reading<Lsn>::invalid("not an LSN: '" + sanitize_message(*field, 40) + "'");
    }
    return Reading<Lsn>::ok(*parsed);
}

std::optional<bool> boolean(const Field& field) {
    if (field == "t") {
        return true;
    }
    if (field == "f") {
        return false;
    }
    return std::nullopt;
}

/// Byte distance from `from` to `to` as a reading.
Reading<double> lag_bytes(const Reading<Lsn>& from, const Reading<Lsn>& to) {
    if (!from.is_ok() || !to.is_ok()) {
        return Reading<double>::missing("position unavailable");
    }
    const auto bytes = bytes_between(from.measured(), to.measured());
    if (!bytes) {
        return Reading<double>::invalid("positions read out of order");
    }
    return Reading<double>::ok(static_cast<double>(*bytes));
}

std::string field_text(const Field& field) {
    return field.value_or(std::string{});
}

template <typename R>
void set_all(R& reading, const std::string& reason) {
    reading = std::remove_cvref_t<R>::missing(reason);
}

class Collector {
  public:
    Collector(SqlSession& session, PgNodeSnapshot& snapshot, const CollectorClock& clock)
        : session_(&session), snapshot_(&snapshot), clock_(&clock) {}

    /// Runs `sql`; on success checks the column count (and, when `single_row`, exactly one
    /// row). On failure records the error and returns nullopt.
    std::optional<SqlTable> run(std::string_view sql, std::size_t columns, bool single_row) {
        SqlOutcome outcome = session_->query(sql);
        if (auto* error = std::get_if<SqlError>(&outcome)) {
            snapshot_->errors.push_back(std::move(*error));
            restart_transaction();
            return std::nullopt;
        }
        auto& table = std::get<SqlTable>(outcome);
        const bool shape_ok =
            table.columns.size() == columns && (!single_row || table.rows.size() == 1) &&
            std::ranges::all_of(table.rows, [&](const auto& row) { return row.size() == columns; });
        if (!shape_ok) {
            snapshot_->errors.push_back(
                SqlError{SqlErrorKind::kUnexpectedResult,
                         {},
                         "unexpected result shape for: " + std::string(sql.substr(0, 60))});
            return std::nullopt;
        }
        return std::move(table);
    }

    /// All queries of one collection share a read-only transaction: the statistics views are
    /// then read from one consistent snapshot (stats_fetch_consistency = cache), and the
    /// collector adds exactly one transaction per cycle to the node's counters.
    void begin() {
        SqlOutcome outcome =
            session_->query("BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ READ ONLY");
        in_transaction_ = std::holds_alternative<SqlTable>(outcome);
        if (auto* error = std::get_if<SqlError>(&outcome)) {
            // Kept: on a frozen server this is the first (and most telling) failure.
            snapshot_->errors.push_back(std::move(*error));
        }
    }

    void finish() {
        // PostgreSQL 15+ flushes a backend's statistics lazily (up to seconds later). Forcing
        // the flush makes this collection's own commit visible before the next cycle reads the
        // counters, so compute_rates can subtract it exactly instead of on average.
        constexpr double kFirstVersionWithForceFlush = 150000.0;
        if (in_transaction_ && snapshot_->server_version_num.is_ok() &&
            snapshot_->server_version_num.measured() >= kFirstVersionWithForceFlush) {
            (void)run(kForceStatsFlushSql, 1, true);
        }
        if (in_transaction_ && std::holds_alternative<SqlTable>(session_->query("COMMIT"))) {
            ++snapshot_->own_commits;
        }
        in_transaction_ = false;
    }

    /// After a failed statement the transaction is aborted; roll it back and start a new one so
    /// that the remaining queries still run.
    void restart_transaction() {
        if (!in_transaction_) {
            return;
        }
        if (std::holds_alternative<SqlTable>(session_->query("ROLLBACK"))) {
            ++snapshot_->own_rollbacks;
        }
        begin();
    }

    [[nodiscard]] std::string last_error_reason() const {
        if (snapshot_->errors.empty()) {
            return "query failed";
        }
        return "query failed: " + std::string(to_string(snapshot_->errors.back().kind));
    }

    void identity() {
        const auto table = run(kIdentitySql, 4, true);
        if (!table) {
            const std::string reason = last_error_reason();
            snapshot_->server_version_num = Reading<double>::missing(reason);
            snapshot_->connections.max_connections = Reading<double>::missing(reason);
            return;
        }
        const auto& row = table->rows.front();
        const auto in_recovery = boolean(row[0]);
        if (in_recovery) {
            snapshot_->role = *in_recovery ? NodeRole::kReplica : NodeRole::kPrimary;
        } else {
            snapshot_->errors.push_back(SqlError{
                SqlErrorKind::kUnexpectedResult, {}, "pg_is_in_recovery() returned a non-boolean"});
        }
        snapshot_->server_version_num = number(row[1]);
        snapshot_->cluster_name = sanitize_message(field_text(row[2]), 64);
        snapshot_->connections.max_connections = number(row[3]);
    }

    void probe() {
        const SteadyTime start = clock_->steady();
        const auto table = run(kProbeSql, 1, true);
        const SteadyTime end = clock_->steady();
        snapshot_->probe_latency_ms = table ? Reading<double>::ok(elapsed_ms(start, end))
                                            : Reading<double>::missing(last_error_reason());
    }

    void connections() {
        auto& c = snapshot_->connections;
        const auto table = run(kConnectionsSql, 4, true);
        if (!table) {
            const std::string reason = last_error_reason();
            set_all(c.client_backends, reason);
            set_all(c.active, reason);
            set_all(c.idle_in_transaction, reason);
            set_all(c.waiting_on_lock, reason);
            set_all(c.utilization_percent, reason);
            return;
        }
        const auto& row = table->rows.front();
        c.client_backends = number(row[0]);
        c.active = number(row[1]);
        c.idle_in_transaction = number(row[2]);
        c.waiting_on_lock = number(row[3]);
        if (c.client_backends.is_ok() && c.max_connections.is_ok() &&
            c.max_connections.measured() > 0.0) {
            c.utilization_percent = Reading<double>::ok(100.0 * c.client_backends.measured() /
                                                        c.max_connections.measured());
        } else {
            c.utilization_percent =
                Reading<double>::missing("needs client_backends and max_connections");
        }
    }

    void transactions() {
        const auto table = run(kTransactionsSql, 2, true);
        if (!table) {
            const std::string reason = last_error_reason();
            set_all(snapshot_->xact_commit_total, reason);
            set_all(snapshot_->xact_rollback_total, reason);
            return;
        }
        snapshot_->xact_commit_total = number(table->rows.front()[0]);
        snapshot_->xact_rollback_total = number(table->rows.front()[1]);
    }

    void statements() {
        // Checked first so that a missing extension is a reading quality, not an error that
        // aborts the collection transaction every cycle.
        const auto present = run(kStatementsPresentSql, 1, true);
        if (present && boolean(present->rows.front()[0]) == false) {
            const std::string reason = "pg_stat_statements is not installed";
            set_all(snapshot_->statements_calls_total, reason);
            set_all(snapshot_->statements_exec_time_ms_total, reason);
            return;
        }
        const auto table = present ? run(kStatementsSql, 2, true) : std::nullopt;
        if (!table) {
            const std::string reason = last_error_reason();
            set_all(snapshot_->statements_calls_total, reason);
            set_all(snapshot_->statements_exec_time_ms_total, reason);
            return;
        }
        snapshot_->statements_calls_total = number(table->rows.front()[0]);
        snapshot_->statements_exec_time_ms_total = number(table->rows.front()[1]);
    }

    void primary_side() {
        if (const auto table = run(kPrimaryWalSql, 1, true)) {
            snapshot_->current_wal_lsn = lsn(table->rows.front()[0]);
        } else {
            snapshot_->current_wal_lsn = Reading<Lsn>::missing(last_error_reason());
        }

        if (const auto table = run(kStandbysSql, 11, false)) {
            for (const auto& row : table->rows) {
                StandbyLink link;
                link.application_name = sanitize_message(field_text(row[0]), 64);
                link.client_addr = sanitize_message(field_text(row[1]), 64);
                link.state = sanitize_message(field_text(row[2]), 32);
                link.sync_state = sanitize_message(field_text(row[3]), 32);
                link.sent_lsn = lsn(row[4]);
                link.flush_lsn = lsn(row[5]);
                link.replay_lsn = lsn(row[6], "standby has not reported a replay position");
                constexpr std::string_view kIdle =
                    "NULL: no recent WAL activity (PostgreSQL clears lag when idle)";
                link.write_lag_seconds = number(row[7], kIdle);
                link.flush_lag_seconds = number(row[8], kIdle);
                link.replay_lag_seconds = number(row[9], kIdle);
                link.replay_lag_bytes = lag_bytes(link.replay_lsn, lsn(row[10]));
                snapshot_->standbys.push_back(std::move(link));
            }
            snapshot_->standby_count =
                Reading<double>::ok(static_cast<double>(snapshot_->standbys.size()));
        } else {
            snapshot_->standby_count = Reading<double>::missing(last_error_reason());
        }

        if (const auto table = run(kSlotsSql, 5, false)) {
            for (const auto& row : table->rows) {
                ReplicationSlot slot;
                slot.name = sanitize_message(field_text(row[0]), 64);
                slot.active = boolean(row[1]).value_or(false);
                slot.wal_status = sanitize_message(field_text(row[2]), 32);
                slot.retained_bytes =
                    lag_bytes(lsn(row[3], "slot has never reserved WAL"), lsn(row[4]));
                snapshot_->slots.push_back(std::move(slot));
            }
            snapshot_->slot_count =
                Reading<double>::ok(static_cast<double>(snapshot_->slots.size()));
        } else {
            snapshot_->slot_count = Reading<double>::missing(last_error_reason());
        }
    }

    void replica_side(const CollectionPolicy& policy) {
        auto& r = snapshot_->replica;
        const ReplayAge replay_age = replica_positions();
        r.last_replay_age_seconds = last_replay_age(replay_age);

        // After a WAL receiver (re)starts it reports the segment-aligned start of streaming as
        // its receive position, which can be *behind* the replay position until new WAL
        // arrives (observed in the lab after restarting a replica). Nothing is then pending.
        const bool receive_behind_replay = r.receive_lsn.is_ok() && r.replay_lsn.is_ok() &&
                                           r.receive_lsn.measured() < r.replay_lsn.measured();
        r.apply_backlog_bytes =
            receive_behind_replay
                ? Reading<double>::ok(0.0, "receive position behind replay (receiver restarted)")
                : lag_bytes(r.replay_lsn, r.receive_lsn);

        const bool streaming = wal_receiver();
        r.replay_lag_seconds = replica_replay_lag(r, streaming);
        apply_receiver_silence(r, policy);
    }

  private:
    struct ReplayAge {
        bool null{true};                ///< pg_last_xact_replay_timestamp() was NULL
        std::optional<double> seconds;  ///< parsed age when not NULL
    };

    ReplayAge replica_positions() {
        auto& r = snapshot_->replica;
        ReplayAge age;
        const auto table = run(kReplicaSql, 4, true);
        if (!table) {
            const std::string reason = last_error_reason();
            set_all(r.receive_lsn, reason);
            set_all(r.replay_lsn, reason);
            set_all(r.replay_paused, reason);
            return age;
        }
        const auto& row = table->rows.front();
        r.receive_lsn = lsn(row[0], "nothing received from the primary yet");
        r.replay_lsn = lsn(row[1]);
        if (const auto& replay_age = row[2]; replay_age.has_value()) {
            age.null = false;
            age.seconds = parse_finite(replay_age.value());
        }
        const auto paused = boolean(row[3]);
        r.replay_paused = paused ? Reading<bool>::ok(*paused)
                                 : Reading<bool>::invalid("non-boolean replay pause state");
        return age;
    }

    /// Age of the last replayed commit, reported as what it is.
    static Reading<double> last_replay_age(const ReplayAge& age) {
        if (age.null) {
            return Reading<double>::missing("no commit replayed yet");
        }
        if (!age.seconds) {
            return Reading<double>::invalid("unparseable replay age");
        }
        if (*age.seconds < 0.0) {
            return Reading<double>::invalid("negative replay age (clock skew between nodes)");
        }
        return Reading<double>::ok(*age.seconds);
    }

    /// Reads pg_stat_wal_receiver; returns true when the receiver is streaming.
    bool wal_receiver() {
        auto& r = snapshot_->replica;
        const auto table = run(kReceiverSql, 3, false);
        if (!table) {
            const std::string reason = last_error_reason();
            set_all(r.receiver_status, reason);
            set_all(r.last_message_age_seconds, reason);
            return false;
        }
        if (table->rows.empty()) {
            r.receiver_status = Reading<std::string>::missing("no WAL receiver running");
            r.last_message_age_seconds = Reading<double>::missing("no WAL receiver running");
            return false;
        }
        const auto& row = table->rows.front();
        const auto& status = row[0];
        r.receiver_status = status.has_value()
                                ? Reading<std::string>::ok(sanitize_message(status.value(), 32))
                                : Reading<std::string>::missing("NULL from server");
        r.sender_host = sanitize_message(field_text(row[1]), 64);
        r.last_message_age_seconds = number(row[2]);
        return r.receiver_status.is_ok() && r.receiver_status.measured() == "streaming";
    }

    /// Replay lag in seconds, replica view. The common recipe
    /// now() - pg_last_xact_replay_timestamp() is not used: on an idle or commit-free primary
    /// it reports minutes of "lag" for a replica that is fully caught up (observed in the lab:
    /// 474 s with a 3.9 KB backlog of checkpoint records). Only "caught up" is certain.
    static Reading<double> replica_replay_lag(const ReplicaState& r, bool streaming) {
        if (!r.receive_lsn.is_ok() || !r.replay_lsn.is_ok()) {
            return Reading<double>::missing("WAL positions unavailable");
        }
        if (!streaming) {
            return Reading<double>::missing("WAL receiver not streaming; lag vs primary unknown");
        }
        if (r.receive_lsn.measured() <= r.replay_lsn.measured()) {
            return Reading<double>::ok(0.0, "caught up with received WAL");
        }
        return Reading<double>::missing(
            "WAL pending replay; use apply_backlog_bytes or the primary's replay_lag");
    }

    static void apply_receiver_silence(ReplicaState& r, const CollectionPolicy& policy) {
        if (!r.last_message_age_seconds.is_ok()) {
            return;
        }
        const double silence = r.last_message_age_seconds.measured();
        const double limit = std::chrono::duration<double>(policy.max_receiver_silence).count();
        if (silence > limit) {
            const std::string reason = "no message from the primary for " +
                                       std::to_string(static_cast<long long>(silence)) + " s";
            r.apply_backlog_bytes.mark_stale(reason);
            r.replay_lag_seconds.mark_stale(reason);
        }
    }

    SqlSession* session_;
    PgNodeSnapshot* snapshot_;
    const CollectorClock* clock_;
    bool in_transaction_{false};
};

void mark_role_specific(PgNodeSnapshot& s) {
    const auto primary_na = [](auto& reading) {
        reading = std::remove_cvref_t<decltype(reading)>::not_applicable("primary");
    };
    const auto replica_na = [](auto& reading) {
        reading = std::remove_cvref_t<decltype(reading)>::not_applicable("replica");
    };
    if (s.role == NodeRole::kPrimary) {
        primary_na(s.replica.receiver_status);
        primary_na(s.replica.receive_lsn);
        primary_na(s.replica.replay_lsn);
        primary_na(s.replica.apply_backlog_bytes);
        primary_na(s.replica.replay_lag_seconds);
        primary_na(s.replica.last_replay_age_seconds);
        primary_na(s.replica.last_message_age_seconds);
        primary_na(s.replica.replay_paused);
    } else if (s.role == NodeRole::kReplica) {
        replica_na(s.current_wal_lsn);
        replica_na(s.standby_count);
        replica_na(s.slot_count);
    } else {
        const std::string reason = "role unknown";
        for_each_reading(s, [&](std::string_view name, auto& reading) {
            if (name == "current_wal_lsn" || name == "standby_count" || name == "slot_count" ||
                name.starts_with("replica.")) {
                set_all(reading, reason);
            }
        });
    }
}

template <typename Fn>
void for_each_list_reading(PgNodeSnapshot& s, const Fn& fn) {
    for (auto& link : s.standbys) {
        fn(link.sent_lsn);
        fn(link.flush_lsn);
        fn(link.replay_lsn);
        fn(link.replay_lag_bytes);
        fn(link.write_lag_seconds);
        fn(link.flush_lag_seconds);
        fn(link.replay_lag_seconds);
    }
    for (auto& slot : s.slots) {
        fn(slot.retained_bytes);
    }
}

}  // namespace

CollectorClock CollectorClock::system() {
    return CollectorClock{[] { return telemetry::Clock::now(); },
                          [] { return std::chrono::steady_clock::now(); }};
}

std::string_view to_string(Availability availability) noexcept {
    switch (availability) {
        case Availability::kUp:
            return "up";
        case Availability::kDegraded:
            return "degraded";
        case Availability::kDown:
            return "down";
    }
    return "unknown";
}

PgNodeSnapshot collect_pg_node(SqlSession& session, std::string node, const CollectorClock& clock,
                               const CollectionPolicy& policy, Reading<double> connect_latency_ms) {
    PgNodeSnapshot snapshot;
    snapshot.node = std::move(node);
    snapshot.attempted_at = clock.wall();
    snapshot.connect_latency_ms = std::move(connect_latency_ms);

    Collector collector(session, snapshot, clock);
    collector.begin();
    collector.identity();
    collector.probe();
    collector.connections();
    collector.transactions();
    collector.statements();
    if (snapshot.role == NodeRole::kPrimary) {
        collector.primary_side();
    } else if (snapshot.role == NodeRole::kReplica) {
        collector.replica_side(policy);
    }
    collector.finish();
    mark_role_specific(snapshot);

    snapshot.availability = snapshot.errors.empty() ? Availability::kUp : Availability::kDegraded;
    snapshot.collected_at = clock.wall();
    snapshot.collected_steady = clock.steady();
    return snapshot;
}

PgNodeSnapshot unreachable_snapshot(std::string node, Timestamp attempted_at, const SqlError& error,
                                    const CollectorClock& clock) {
    PgNodeSnapshot snapshot;
    snapshot.node = std::move(node);
    snapshot.attempted_at = attempted_at;
    snapshot.availability = Availability::kDown;
    snapshot.errors.push_back(error);
    const std::string reason = "node unreachable: " + std::string(to_string(error.kind));
    for_each_reading(snapshot, [&](std::string_view, auto& reading) { set_all(reading, reason); });
    snapshot.collected_at = clock.wall();
    snapshot.collected_steady = clock.steady();
    return snapshot;
}

bool mark_stale_if_older(PgNodeSnapshot& snapshot, Timestamp now,
                         std::chrono::milliseconds max_age) {
    const auto age = now - snapshot.collected_at;
    if (age <= max_age) {
        return false;
    }
    const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(age).count();
    const std::string reason = "snapshot is " + std::to_string(age_ms) + " ms old (limit " +
                               std::to_string(max_age.count()) + " ms)";
    for_each_reading(snapshot,
                     [&](std::string_view, auto& reading) { reading.mark_stale(reason); });
    for_each_list_reading(snapshot, [&](auto& reading) { reading.mark_stale(reason); });
    return true;
}

Coverage coverage(const PgNodeSnapshot& snapshot) {
    Coverage result;
    for_each_reading(snapshot, [&](std::string_view, const auto& reading) {
        if (reading.quality() == Quality::kNotApplicable) {
            return;
        }
        ++result.applicable;
        if (reading.is_ok()) {
            ++result.ok;
        }
    });
    return result;
}

}  // namespace faulttwin::collectors
