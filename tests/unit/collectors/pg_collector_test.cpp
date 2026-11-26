#include <gtest/gtest.h>

#include <string>

#include "fake_sql_session.hpp"
#include "faulttwin/collectors/pg_snapshot.hpp"

namespace faulttwin::collectors {
namespace {

using testing::FakeClocks;
using testing::FakeSqlSession;
using testing::script_primary;
using testing::script_replica;
using testing::table;

TEST(PgCollectorTest, HealthyPrimary) {
    FakeSqlSession session;
    script_primary(session);
    FakeClocks clocks;
    const auto snap = collect_pg_node(session, "pg-primary", clocks.clock());

    EXPECT_EQ(snap.availability, Availability::kUp);
    EXPECT_TRUE(snap.errors.empty());
    EXPECT_EQ(snap.role, NodeRole::kPrimary);
    EXPECT_EQ(snap.cluster_name, "pg-primary");
    EXPECT_EQ(*snap.server_version_num.value(), 170012.0);
    EXPECT_EQ(*snap.connections.client_backends.value(), 7.0);
    EXPECT_EQ(*snap.connections.utilization_percent.value(), 7.0);
    EXPECT_EQ(*snap.xact_commit_total.value(), 1500.0);
    EXPECT_EQ(*snap.statements_calls_total.value(), 200.0);
    EXPECT_TRUE(snap.probe_latency_ms.is_ok());
    EXPECT_EQ(snap.current_wal_lsn.value()->value, 0x3000100U);

    ASSERT_EQ(snap.standbys.size(), 2U);
    EXPECT_EQ(snap.standbys[0].application_name, "pg-replica-1");
    EXPECT_EQ(*snap.standbys[0].replay_lag_bytes.value(), 0x100);
    EXPECT_EQ(*snap.standbys[0].replay_lag_seconds.value(), 0.003);
    // NULL lag columns on an idle, caught-up standby are missing, never zero.
    EXPECT_EQ(snap.standbys[1].replay_lag_seconds.quality(), Quality::kMissing);
    EXPECT_NE(snap.standbys[1].replay_lag_seconds.detail().find("idle"), std::string::npos);
    EXPECT_EQ(*snap.standbys[1].replay_lag_bytes.value(), 0.0);

    ASSERT_EQ(snap.slots.size(), 2U);
    EXPECT_TRUE(snap.slots[0].active);
    EXPECT_EQ(*snap.slots[0].retained_bytes.value(), 0x100);

    // Replica-only metrics do not apply to a primary.
    EXPECT_EQ(snap.replica.replay_lag_seconds.quality(), Quality::kNotApplicable);
    EXPECT_EQ(snap.replica.receiver_status.quality(), Quality::kNotApplicable);
}

TEST(PgCollectorTest, RunsInOneReadOnlyTransactionAndCountsItsOwnCommit) {
    FakeSqlSession session;
    script_primary(session);
    FakeClocks clocks;
    const auto snap = collect_pg_node(session, "pg-primary", clocks.clock());
    ASSERT_FALSE(session.log.empty());
    EXPECT_EQ(session.log.front(), "BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ READ ONLY");
    EXPECT_EQ(session.log.back(), "COMMIT");
    EXPECT_EQ(session.count("pg_stat_force_next_flush"), 1);
    EXPECT_EQ(snap.own_commits, 1U);
    EXPECT_EQ(snap.own_rollbacks, 0U);
}

TEST(PgCollectorTest, CaughtUpReplica) {
    FakeSqlSession session;
    script_replica(session);
    FakeClocks clocks;
    const auto snap = collect_pg_node(session, "pg-replica-1", clocks.clock());
    EXPECT_EQ(snap.availability, Availability::kUp);
    EXPECT_EQ(snap.role, NodeRole::kReplica);
    EXPECT_EQ(*snap.replica.receiver_status.value(), "streaming");
    EXPECT_EQ(snap.replica.sender_host, "pg-primary");
    EXPECT_EQ(*snap.replica.apply_backlog_bytes.value(), 0.0);
    ASSERT_TRUE(snap.replica.replay_lag_seconds.is_ok());
    EXPECT_EQ(*snap.replica.replay_lag_seconds.value(), 0.0);
    // The age of the last replayed commit is reported separately and is not lag.
    EXPECT_EQ(*snap.replica.last_replay_age_seconds.value(), 600.5);
    EXPECT_FALSE(*snap.replica.replay_paused.value());
    EXPECT_EQ(snap.current_wal_lsn.quality(), Quality::kNotApplicable);
    EXPECT_EQ(snap.standby_count.quality(), Quality::kNotApplicable);
}

TEST(PgCollectorTest, ReplicaWithPendingWalHasBacklogButNoTimeLag) {
    FakeSqlSession session;
    session.on("pg_last_wal_receive_lsn", table(4, {{"0/3001000", "0/3000100", "474.4", "f"}}));
    script_replica(session);
    FakeClocks clocks;
    const auto snap = collect_pg_node(session, "pg-replica-1", clocks.clock());
    EXPECT_EQ(*snap.replica.apply_backlog_bytes.value(), 0xF00);
    // 474 s since the last commit is not reported as replay lag.
    EXPECT_EQ(snap.replica.replay_lag_seconds.quality(), Quality::kMissing);
    EXPECT_EQ(*snap.replica.last_replay_age_seconds.value(), 474.4);
}

TEST(PgCollectorTest, ReceivePositionBehindReplayAfterReceiverRestartIsCaughtUp) {
    FakeSqlSession session;
    session.on("pg_last_wal_receive_lsn",
               table(4, {{"0/3000000", "0/3000100", std::nullopt, "f"}}));
    script_replica(session);
    FakeClocks clocks;
    const auto snap = collect_pg_node(session, "pg-replica-1", clocks.clock());
    ASSERT_TRUE(snap.replica.apply_backlog_bytes.is_ok());
    EXPECT_EQ(*snap.replica.apply_backlog_bytes.value(), 0.0);
    EXPECT_EQ(*snap.replica.replay_lag_seconds.value(), 0.0);
    // No commit replayed since the restart: missing, not 0 s and not "now".
    EXPECT_EQ(snap.replica.last_replay_age_seconds.quality(), Quality::kMissing);
}

TEST(PgCollectorTest, ReplicaWithoutWalReceiver) {
    FakeSqlSession session;
    session.on("FROM pg_stat_wal_receiver", table(3, {}));
    script_replica(session);
    FakeClocks clocks;
    const auto snap = collect_pg_node(session, "pg-replica-1", clocks.clock());
    EXPECT_EQ(snap.availability, Availability::kUp);  // reachable; the problem is replication
    EXPECT_EQ(snap.replica.receiver_status.quality(), Quality::kMissing);
    EXPECT_EQ(snap.replica.replay_lag_seconds.quality(), Quality::kMissing);
    EXPECT_NE(snap.replica.replay_lag_seconds.detail().find("not streaming"), std::string::npos);
}

TEST(PgCollectorTest, SilentReceiverMakesReceiveDerivedReadingsStale) {
    FakeSqlSession session;
    session.on("FROM pg_stat_wal_receiver", table(3, {{"streaming", "pg-primary", "45"}}));
    script_replica(session);
    FakeClocks clocks;
    CollectionPolicy policy;
    policy.max_receiver_silence = std::chrono::seconds{30};
    const auto snap = collect_pg_node(session, "pg-replica-1", clocks.clock(), policy);
    EXPECT_EQ(snap.replica.apply_backlog_bytes.quality(), Quality::kStale);
    EXPECT_EQ(snap.replica.replay_lag_seconds.quality(), Quality::kStale);
    EXPECT_EQ(*snap.replica.replay_lag_seconds.value(), 0.0);
    EXPECT_TRUE(snap.replica.last_message_age_seconds.is_ok());
}

TEST(PgCollectorTest, MissingPgStatStatementsIsAQualityNotAnError) {
    FakeSqlSession session;
    session.on("to_regclass", table(1, {{"f"}}));
    script_primary(session);
    FakeClocks clocks;
    const auto snap = collect_pg_node(session, "pg-primary", clocks.clock());
    EXPECT_EQ(snap.availability, Availability::kUp);
    EXPECT_EQ(snap.statements_calls_total.quality(), Quality::kMissing);
    EXPECT_EQ(snap.statements_calls_total.detail(), "pg_stat_statements is not installed");
    EXPECT_EQ(session.count("FROM pg_stat_statements"), 0);
}

TEST(PgCollectorTest, PermissionDeniedDegradesAndRestartsTheTransaction) {
    FakeSqlSession session;
    session.on("FROM pg_stat_activity",
               SqlError{SqlErrorKind::kPermissionDenied, "42501", "permission denied"});
    script_primary(session);
    FakeClocks clocks;
    const auto snap = collect_pg_node(session, "pg-primary", clocks.clock());
    EXPECT_EQ(snap.availability, Availability::kDegraded);
    ASSERT_EQ(snap.errors.size(), 1U);
    EXPECT_EQ(snap.errors[0].kind, SqlErrorKind::kPermissionDenied);
    EXPECT_EQ(snap.connections.client_backends.quality(), Quality::kMissing);
    EXPECT_EQ(snap.connections.client_backends.detail(), "query failed: permission_denied");
    // Later queries still ran in a fresh transaction.
    EXPECT_TRUE(snap.xact_commit_total.is_ok());
    EXPECT_EQ(session.count("ROLLBACK"), 1);
    EXPECT_EQ(session.count("BEGIN"), 2);
    EXPECT_EQ(snap.own_rollbacks, 1U);
}

TEST(PgCollectorTest, GarbageValuesAreInvalidNotZero) {
    FakeSqlSession session;
    session.on("FROM pg_stat_activity", table(4, {{"-1", "abc", "1e400", std::nullopt}}));
    session.on("SELECT pg_current_wal_lsn()", table(1, {{"not-an-lsn"}}));
    script_primary(session);
    FakeClocks clocks;
    const auto snap = collect_pg_node(session, "pg-primary", clocks.clock());
    EXPECT_EQ(snap.connections.client_backends.quality(), Quality::kInvalid);
    EXPECT_EQ(snap.connections.active.quality(), Quality::kInvalid);
    EXPECT_EQ(snap.connections.idle_in_transaction.quality(), Quality::kInvalid);
    EXPECT_EQ(snap.connections.waiting_on_lock.quality(), Quality::kMissing);
    EXPECT_EQ(snap.connections.utilization_percent.quality(), Quality::kMissing);
    EXPECT_EQ(snap.current_wal_lsn.quality(), Quality::kInvalid);
}

TEST(PgCollectorTest, UnexpectedResultShapeIsReported) {
    FakeSqlSession session;
    session.on("FROM pg_stat_database", table(2, {}));  // zero rows
    script_primary(session);
    FakeClocks clocks;
    const auto snap = collect_pg_node(session, "pg-primary", clocks.clock());
    EXPECT_EQ(snap.availability, Availability::kDegraded);
    ASSERT_EQ(snap.errors.size(), 1U);
    EXPECT_EQ(snap.errors[0].kind, SqlErrorKind::kUnexpectedResult);
    EXPECT_EQ(snap.xact_commit_total.quality(), Quality::kMissing);
}

TEST(PgCollectorTest, UnreadableRoleLeavesRoleUnknown) {
    FakeSqlSession session;
    session.on("pg_is_in_recovery()", SqlError{SqlErrorKind::kTimeout, "57014", "timeout"});
    script_primary(session);
    FakeClocks clocks;
    const auto snap = collect_pg_node(session, "pg-primary", clocks.clock());
    EXPECT_EQ(snap.role, NodeRole::kUnknown);
    EXPECT_EQ(snap.availability, Availability::kDegraded);
    EXPECT_EQ(snap.current_wal_lsn.quality(), Quality::kMissing);
    EXPECT_EQ(snap.replica.replay_lag_seconds.quality(), Quality::kMissing);
    EXPECT_EQ(session.count("pg_stat_replication"), 0);
}

TEST(PgCollectorTest, UnreachableSnapshotIsAllMissing) {
    FakeClocks clocks;
    const SqlError error{SqlErrorKind::kConnectionFailed, {}, "connection refused"};
    const auto snap = unreachable_snapshot("pg-replica-2", clocks.wall_now, error, clocks.clock());
    EXPECT_EQ(snap.availability, Availability::kDown);
    EXPECT_EQ(snap.role, NodeRole::kUnknown);
    int readings = 0;
    for_each_reading(snap, [&](std::string_view name, const auto& r) {
        ++readings;
        EXPECT_EQ(r.quality(), Quality::kMissing) << name;
        EXPECT_EQ(r.detail(), "node unreachable: connection_failed") << name;
    });
    EXPECT_GT(readings, 20);
    const auto cov = coverage(snap);
    EXPECT_EQ(cov.ok, 0);
    EXPECT_EQ(cov.applicable, readings);
}

TEST(PgCollectorTest, StalenessIsSeparateFromMissing) {
    FakeSqlSession session;
    script_replica(session);
    FakeClocks clocks;
    auto snap = collect_pg_node(session, "pg-replica-1", clocks.clock());
    const auto collected = snap.collected_at;

    EXPECT_FALSE(
        mark_stale_if_older(snap, collected + std::chrono::seconds{5}, std::chrono::seconds{5}));
    EXPECT_TRUE(snap.probe_latency_ms.is_ok());

    EXPECT_TRUE(
        mark_stale_if_older(snap, collected + std::chrono::seconds{6}, std::chrono::seconds{5}));
    EXPECT_EQ(snap.probe_latency_ms.quality(), Quality::kStale);
    EXPECT_TRUE(snap.probe_latency_ms.value().has_value());
    EXPECT_EQ(snap.current_wal_lsn.quality(), Quality::kNotApplicable);  // unchanged
    EXPECT_EQ(coverage(snap).ok, 0);
}

TEST(PgCollectorTest, CoverageCountsOnlyApplicableReadings) {
    FakeSqlSession session;
    script_primary(session);
    FakeClocks clocks;
    const auto snap = collect_pg_node(session, "pg-primary", clocks.clock());
    const auto cov = coverage(snap);
    int total = 0;
    for_each_reading(snap, [&](std::string_view, const auto&) { ++total; });
    EXPECT_EQ(cov.ok, cov.applicable);
    // Replica-only readings and the connect latency of a reused connection do not apply.
    EXPECT_EQ(cov.applicable, total - 9);
}

}  // namespace
}  // namespace faulttwin::collectors
