#include <gtest/gtest.h>

#include <vector>

#include "fake_sql_session.hpp"
#include "faulttwin/collectors/rates.hpp"
#include "faulttwin/collectors/topology.hpp"

namespace faulttwin::collectors {
namespace {

using testing::FakeClocks;
using testing::FakeSqlSession;
using testing::table;

PgNodeSnapshot primary_with(double commits, double calls, double exec_ms, const char* lsn,
                            FakeClocks& clocks) {
    FakeSqlSession session;
    session.on("FROM pg_stat_database", table(2, {{std::to_string(commits), "0"}}));
    session.on("FROM pg_stat_statements",
               table(2, {{std::to_string(calls), std::to_string(exec_ms)}}));
    session.on("SELECT pg_current_wal_lsn()", table(1, {{lsn}}));
    testing::script_primary(session);
    return collect_pg_node(session, "pg-primary", clocks.clock());
}

TEST(RatesTest, ComputesPerSecondRatesAndSubtractsTheCollectorsOwnCommit) {
    FakeClocks clocks;
    const auto a = primary_with(1000, 100, 50, "0/1000000", clocks);
    clocks.steady_now += std::chrono::milliseconds{2000 - 1};
    const auto b = primary_with(1201, 140, 70, "0/1100000", clocks);
    const auto r = compute_rates(a, b);
    ASSERT_TRUE(r.interval_seconds.is_ok());
    const double seconds = *r.interval_seconds.value();
    EXPECT_NEAR(seconds, 2.0, 0.05);
    // 201 commits, one of which was the collector's own transaction.
    EXPECT_NEAR(*r.commits_per_second.value(), 200.0 / seconds, 1e-9);
    EXPECT_NEAR(*r.wal_bytes_per_second.value(), 0x100000 / seconds, 1e-6);
    EXPECT_NEAR(*r.mean_statement_latency_ms.value(), 0.5, 1e-9);
}

TEST(RatesTest, CounterResetIsInvalidNotNegative) {
    FakeClocks clocks;
    const auto a = primary_with(1000, 100, 50, "0/1000000", clocks);
    const auto b = primary_with(10, 5, 1, "0/1000000", clocks);
    const auto r = compute_rates(a, b);
    EXPECT_EQ(r.commits_per_second.quality(), Quality::kInvalid);
    EXPECT_EQ(r.mean_statement_latency_ms.quality(), Quality::kInvalid);
    EXPECT_EQ(*r.wal_bytes_per_second.value(), 0.0);
}

TEST(RatesTest, NoStatementsInIntervalIsMissingNotZeroLatency) {
    FakeClocks clocks;
    const auto a = primary_with(1000, 100, 50, "0/1000000", clocks);
    const auto b = primary_with(1001, 100, 50, "0/1000000", clocks);
    const auto r = compute_rates(a, b);
    EXPECT_EQ(r.mean_statement_latency_ms.quality(), Quality::kMissing);
    EXPECT_EQ(*r.commits_per_second.value(), 0.0);  // only the collector committed
}

TEST(RatesTest, WalMovingBackwardsIsInvalid) {
    FakeClocks clocks;
    const auto a = primary_with(1000, 100, 50, "0/2000000", clocks);
    const auto b = primary_with(1000, 100, 50, "0/1000000", clocks);
    EXPECT_EQ(compute_rates(a, b).wal_bytes_per_second.quality(), Quality::kInvalid);
}

TEST(RatesTest, DifferentNodesOrZeroIntervalAreInvalid) {
    FakeClocks clocks;
    auto a = primary_with(1000, 100, 50, "0/1000000", clocks);
    auto b = a;
    EXPECT_EQ(compute_rates(a, b).interval_seconds.quality(), Quality::kInvalid);
    b.node = "other";
    b.collected_steady += std::chrono::seconds{1};
    EXPECT_EQ(compute_rates(a, b).commits_per_second.quality(), Quality::kInvalid);
}

struct Cluster {
    FakeClocks clocks;
    std::vector<PgNodeSnapshot> nodes;

    Cluster() {
        FakeSqlSession p;
        testing::script_primary(p);
        nodes.push_back(collect_pg_node(p, "pg-primary", clocks.clock()));
        for (const char* name : {"pg-replica-1", "pg-replica-2"}) {
            FakeSqlSession r;
            testing::script_replica(r, name);
            nodes.push_back(collect_pg_node(r, name, clocks.clock()));
        }
    }
};

TEST(TopologyTest, HealthyClusterHasOnePrimaryAndTwoEdges) {
    const Cluster c;
    const auto t = derive_topology(c.nodes);
    EXPECT_TRUE(t.healthy());
    EXPECT_EQ(t.primary, "pg-primary");
    EXPECT_EQ(t.replicas, (std::vector<std::string>{"pg-replica-1", "pg-replica-2"}));
    ASSERT_EQ(t.edges.size(), 2U);
    EXPECT_EQ(t.edges[0].downstream, "pg-replica-1");
    EXPECT_EQ(*t.edges[0].replay_lag_bytes.value(), 0x100);
}

TEST(TopologyTest, DownReplicaIsReportedAndPrimaryStillDerived) {
    Cluster c;
    c.nodes[2] = unreachable_snapshot("pg-replica-2", c.clocks.wall_now,
                                      SqlError{SqlErrorKind::kConnectionFailed, {}, "refused"},
                                      c.clocks.clock());
    const auto t = derive_topology(c.nodes);
    EXPECT_EQ(t.primary, "pg-primary");
    ASSERT_FALSE(t.issues.empty());
    EXPECT_EQ(t.issues[0].code, TopologyIssueCode::kNodeDown);
    EXPECT_EQ(t.issues[0].node, "pg-replica-2");
    EXPECT_EQ(t.issues[0].detail, "connection_failed");
}

TEST(TopologyTest, NoReachablePrimary) {
    Cluster c;
    c.nodes[0] =
        unreachable_snapshot("pg-primary", c.clocks.wall_now,
                             SqlError{SqlErrorKind::kTimeout, {}, "timeout"}, c.clocks.clock());
    const auto t = derive_topology(c.nodes);
    EXPECT_FALSE(t.primary.has_value());
    EXPECT_EQ(t.issues.back().code, TopologyIssueCode::kNoReachablePrimary);
}

TEST(TopologyTest, TwoPrimariesAreASplitBrainRisk) {
    Cluster c;
    c.nodes[1].role = NodeRole::kPrimary;
    const auto t = derive_topology(c.nodes);
    EXPECT_FALSE(t.primary.has_value());
    EXPECT_EQ(t.issues.back().code, TopologyIssueCode::kMultiplePrimaries);
    EXPECT_EQ(t.issues.back().detail, "pg-primary,pg-replica-1");
}

TEST(TopologyTest, ReplicaMissingFromPrimaryViewAndInactiveSlot) {
    Cluster c;
    c.nodes[0].standbys.pop_back();
    c.nodes[0].slots[1].active = false;
    const auto t = derive_topology(c.nodes);
    ASSERT_EQ(t.issues.size(), 2U);
    EXPECT_EQ(t.issues[0].code, TopologyIssueCode::kReplicaNotSeenByPrimary);
    EXPECT_EQ(t.issues[0].node, "pg-replica-2");
    EXPECT_EQ(t.issues[1].code, TopologyIssueCode::kSlotInactive);
    EXPECT_EQ(t.issues[1].node, "pg_replica_2");
}

TEST(TopologyTest, UnknownStandbyAndNotStreamingReplica) {
    Cluster c;
    c.nodes[0].standbys[1].application_name = "rogue";
    c.nodes[2].replica.receiver_status = Reading<std::string>::ok("stopping");
    const auto t = derive_topology(c.nodes);
    std::vector<TopologyIssueCode> codes;
    for (const auto& i : t.issues) {
        codes.push_back(i.code);
    }
    EXPECT_EQ(codes, (std::vector<TopologyIssueCode>{TopologyIssueCode::kReplicaNotStreaming,
                                                     TopologyIssueCode::kUnknownStandby,
                                                     TopologyIssueCode::kReplicaNotSeenByPrimary}));
}

TEST(TopologyTest, IssueCodeStrings) {
    EXPECT_EQ(to_string(TopologyIssueCode::kNodeDown), "node_down");
    EXPECT_EQ(to_string(TopologyIssueCode::kMultiplePrimaries), "multiple_primaries");
    EXPECT_EQ(to_string(TopologyIssueCode::kSlotInactive), "slot_inactive");
}

}  // namespace
}  // namespace faulttwin::collectors
