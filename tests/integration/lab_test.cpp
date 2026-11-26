// Integration tests against the Docker Compose lab cluster (compose.yaml).
//
// Tests that need the cluster run only when FT_INTEGRATION=1 and skip otherwise, so a normal
// `ctest` passes on machines without Docker. When FT_INTEGRATION=1 is set, a missing or broken
// cluster is a failure, not a skip. Outage tests stop/pause containers with `docker compose`
// and always restore them.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "faulttwin/collectors/libpq_session.hpp"
#include "faulttwin/collectors/rates.hpp"
#include "faulttwin/collectors/topology.hpp"

namespace faulttwin::collectors {
namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;
using SteadyClock = std::chrono::steady_clock;

std::optional<std::string> env(const char* name) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): tests read the environment from one thread
    const char* value = std::getenv(name);
    return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
}

bool integration_enabled() {
    return env("FT_INTEGRATION") == "1";
}

#define REQUIRE_LAB()                                                            \
    do {                                                                         \
        if (!integration_enabled()) {                                            \
            GTEST_SKIP() << "set FT_INTEGRATION=1 with the lab cluster running " \
                            "(scripts/run-integration-tests.sh)";                \
        }                                                                        \
    } while (false)

struct Lab {
    std::vector<PgEndpoint> nodes;
    PgCredentials monitor;
    PgCredentials workload;

    static Lab load() {
        Lab lab;
        const auto list = env("FT_MONITOR_NODES");
        EXPECT_TRUE(list.has_value()) << "FT_MONITOR_NODES not set";
        auto parsed = parse_endpoint_list(list.value_or(""));
        if (auto* nodes = std::get_if<std::vector<PgEndpoint>>(&parsed)) {
            lab.nodes = *nodes;
        }
        lab.monitor = {env("FT_MONITOR_USER").value_or("ft_monitor"),
                       env("FT_MONITOR_PASSWORD").value_or(""), "postgres"};
        lab.workload = {env("FT_WORKLOAD_USER").value_or("ft_workload"),
                        env("FT_WORKLOAD_PASSWORD").value_or(""), "postgres"};
        return lab;
    }

    [[nodiscard]] const PgEndpoint& node(const std::string& name) const {
        for (const auto& n : nodes) {
            if (n.node == name) {
                return n;
            }
        }
        ADD_FAILURE() << "node " << name << " not in FT_MONITOR_NODES";
        return nodes.front();
    }

    [[nodiscard]] std::vector<PgNodeSnapshot> collect_all(
        std::vector<PgNodeCollector>& collectors) const {
        std::vector<PgNodeSnapshot> out;
        for (auto& c : collectors) {
            out.push_back(c.collect());
        }
        return out;
    }

    [[nodiscard]] std::vector<PgNodeCollector> collectors(ConnectOptions options = {}) const {
        std::vector<PgNodeCollector> out;
        for (const auto& n : nodes) {
            out.emplace_back(n, monitor, options);
        }
        return out;
    }
};

std::unique_ptr<LibpqSession> connect_or_fail(const PgEndpoint& endpoint,
                                              const PgCredentials& credentials) {
    auto result = LibpqSession::connect(endpoint, credentials, ConnectOptions{});
    if (auto* error = std::get_if<SqlError>(&result)) {
        ADD_FAILURE() << "connect to " << endpoint.node << " failed: " << error->message;
        return nullptr;
    }
    return std::move(std::get<std::unique_ptr<LibpqSession>>(result));
}

std::optional<SqlErrorKind> error_kind(const SqlOutcome& outcome) {
    if (const auto* e = std::get_if<SqlError>(&outcome)) {
        return e->kind;
    }
    return std::nullopt;
}

int compose(const std::string& args) {
    const std::string command =
        "cd '" FAULTTWIN_SOURCE_DIR "' && docker compose " + args + " >/dev/null 2>&1";
    return std::system(command.c_str());  // NOLINT(cert-env33-c, concurrency-mt-unsafe)
}

/// Restores a container on scope exit, whatever the test outcome.
class ComposeGuard {
  public:
    explicit ComposeGuard(std::string restore) : restore_(std::move(restore)) {}
    ComposeGuard(const ComposeGuard&) = delete;
    ComposeGuard& operator=(const ComposeGuard&) = delete;
    ComposeGuard(ComposeGuard&&) = delete;
    ComposeGuard& operator=(ComposeGuard&&) = delete;
    ~ComposeGuard() { (void)compose(restore_); }

  private:
    std::string restore_;
};

const PgNodeSnapshot* find(const std::vector<PgNodeSnapshot>& nodes, const std::string& name) {
    for (const auto& n : nodes) {
        if (n.node == name) {
            return &n;
        }
    }
    return nullptr;
}

/// Like find(), for call sites that need the node to exist: records a failure and returns an
/// empty (down) snapshot instead of a null pointer.
const PgNodeSnapshot& node_of(const std::vector<PgNodeSnapshot>& nodes, const std::string& name) {
    if (const auto* n = find(nodes, name)) {
        return *n;
    }
    ADD_FAILURE() << "no snapshot for " << name;
    static const PgNodeSnapshot kEmpty;
    return kEmpty;
}

bool has_issue(const TopologySnapshot& t, TopologyIssueCode code, const std::string& node) {
    for (const auto& i : t.issues) {
        if (i.code == code && i.node == node) {
            return true;
        }
    }
    return false;
}

/// Polls until `done(snapshots)` or the deadline; returns elapsed time or nullopt.
template <typename Pred>
std::optional<milliseconds> wait_for(const Lab& lab, std::vector<PgNodeCollector>& collectors,
                                     milliseconds limit, Pred done) {
    const auto start = SteadyClock::now();
    while (SteadyClock::now() - start < limit) {
        if (done(lab.collect_all(collectors))) {
            return std::chrono::duration_cast<milliseconds>(SteadyClock::now() - start);
        }
        std::this_thread::sleep_for(milliseconds{200});
    }
    return std::nullopt;
}

bool cluster_healthy(const std::vector<PgNodeSnapshot>& snapshots) {
    const auto t = derive_topology(snapshots);
    return t.healthy() && t.edges.size() == 2;
}

// ---- tests that need only libpq ---------------------------------------------------------

TEST(LibpqSessionTest, RefusedConnectionIsAClassifiedError) {
    // Port 1 on loopback is closed on any normal machine.
    const PgEndpoint endpoint{"nowhere", "127.0.0.1", 1};
    auto result = LibpqSession::connect(endpoint, PgCredentials{"u", "p", "postgres"}, {});
    const auto* error = std::get_if<SqlError>(&result);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(error->kind, SqlErrorKind::kConnectionFailed);
    EXPECT_EQ(error->message.find('\n'), std::string::npos);
    EXPECT_EQ(error->message.find("p\""), std::string::npos);
}

TEST(LibpqSessionTest, CollectorReportsAnUnreachableNodeAsDown) {
    PgNodeCollector collector(PgEndpoint{"nowhere", "127.0.0.1", 1},
                              PgCredentials{"u", "p", "postgres"}, ConnectOptions{});
    const auto snap = collector.collect();
    EXPECT_EQ(snap.availability, Availability::kDown);
    EXPECT_EQ(snap.role, NodeRole::kUnknown);
    EXPECT_EQ(snap.replica.replay_lag_seconds.quality(), Quality::kMissing);
    EXPECT_EQ(coverage(snap).ok, 0);
}

// ---- tests against the lab cluster ------------------------------------------------------

TEST(LabClusterTest, TopologyIsOnePrimaryWithTwoStreamingReplicas) {
    REQUIRE_LAB();
    const Lab lab = Lab::load();
    auto collectors = lab.collectors();
    const auto snapshots = lab.collect_all(collectors);
    for (const auto& s : snapshots) {
        EXPECT_EQ(s.availability, Availability::kUp) << s.node;
        for (const auto& e : s.errors) {
            ADD_FAILURE() << s.node << ": " << to_string(e.kind) << " " << e.message;
        }
        // Everything applicable is ok, except that a replica restarted since its last
        // replayed commit legitimately has no last_replay_age_seconds.
        const auto cov = coverage(s);
        const int allowed = s.replica.last_replay_age_seconds.quality() == Quality::kMissing;
        EXPECT_EQ(cov.ok + allowed, cov.applicable) << s.node;
    }
    const auto t = derive_topology(snapshots);
    for (const auto& i : t.issues) {
        ADD_FAILURE() << "issue " << to_string(i.code) << " " << i.node << " " << i.detail;
    }
    EXPECT_EQ(t.primary, "pg-primary");
    EXPECT_EQ(t.replicas, (std::vector<std::string>{"pg-replica-1", "pg-replica-2"}));
    ASSERT_EQ(t.edges.size(), 2U);
    for (const auto& e : t.edges) {
        EXPECT_EQ(e.state, "streaming");
    }
    const auto* primary = find(snapshots, "pg-primary");
    ASSERT_NE(primary, nullptr);
    EXPECT_GE(*primary->server_version_num.value(), 170000.0);
    EXPECT_EQ(primary->slots.size(), 2U);
}

TEST(LabClusterTest, MonitoringRoleIsReadOnlyAndLeastPrivileged) {
    REQUIRE_LAB();
    const Lab lab = Lab::load();
    auto session = connect_or_fail(lab.node("pg-primary"), lab.monitor);
    ASSERT_NE(session, nullptr);

    EXPECT_EQ(error_kind(session->query("CREATE TABLE ft_probe (x int)")), SqlErrorKind::kReadOnly);
    // Even after lifting the read-only default, the role owns nothing and may create nothing.
    ASSERT_FALSE(error_kind(session->query("SET default_transaction_read_only = off")));
    EXPECT_EQ(error_kind(session->query("CREATE TABLE ft_probe (x int)")),
              SqlErrorKind::kPermissionDenied);
    EXPECT_EQ(error_kind(session->query("CREATE TEMP TABLE ft_probe (x int)")),
              SqlErrorKind::kPermissionDenied);
    EXPECT_EQ(error_kind(session->query("SELECT count(*) FROM lab_app.orders")),
              SqlErrorKind::kPermissionDenied);
    EXPECT_EQ(error_kind(session->query("SELECT pg_read_file('postgresql.conf')")),
              SqlErrorKind::kPermissionDenied);
    EXPECT_EQ(error_kind(session->query("ALTER SYSTEM SET work_mem = '64MB'")),
              SqlErrorKind::kPermissionDenied);

    const auto attrs = session->query(
        "SELECT rolsuper, rolcreaterole, rolcreatedb, rolreplication, rolbypassrls, rolconnlimit "
        "FROM pg_roles WHERE rolname = current_user");
    const auto* t = std::get_if<SqlTable>(&attrs);
    ASSERT_NE(t, nullptr);
    ASSERT_EQ(t->rows.size(), 1U);
    EXPECT_EQ(t->rows[0], (std::vector<std::optional<std::string>>{"f", "f", "f", "f", "f", "5"}));
}

TEST(LabClusterTest, WorkloadShowsUpInRatesAndReplicasCatchUp) {
    REQUIRE_LAB();
    const Lab lab = Lab::load();
    auto collectors = lab.collectors();
    const auto before = lab.collect_all(collectors);

    constexpr int kTransactions = 200;
    {
        auto writer = connect_or_fail(lab.node("pg-primary"), lab.workload);
        ASSERT_NE(writer, nullptr);
        ASSERT_FALSE(error_kind(writer->query("SET default_transaction_read_only = off")));
        for (int i = 0; i < kTransactions; ++i) {
            ASSERT_FALSE(
                error_kind(writer->query("INSERT INTO lab_app.orders (amount) VALUES (12.34)")));
        }
    }  // disconnecting flushes the writer's statistics

    std::this_thread::sleep_for(milliseconds{300});
    const auto after = lab.collect_all(collectors);
    const auto rates = compute_rates(node_of(before, "pg-primary"), node_of(after, "pg-primary"));
    ASSERT_TRUE(rates.commits_per_second.is_ok()) << rates.commits_per_second.detail();
    const double commits = *rates.commits_per_second.value() * *rates.interval_seconds.value();
    EXPECT_GE(commits, kTransactions - 0.5);
    ASSERT_TRUE(rates.wal_bytes_per_second.is_ok());
    EXPECT_GT(*rates.wal_bytes_per_second.value(), 0.0);
    ASSERT_TRUE(rates.mean_statement_latency_ms.is_ok())
        << rates.mean_statement_latency_ms.detail();
    EXPECT_GT(*rates.mean_statement_latency_ms.value(), 0.0);

    const auto& target_lsn = node_of(after, "pg-primary").current_wal_lsn;
    ASSERT_TRUE(target_lsn.is_ok());
    const Lsn target = *target_lsn.value();
    const auto caught_up = wait_for(lab, collectors, seconds{15}, [&](const auto& snaps) {
        for (const char* name : {"pg-replica-1", "pg-replica-2"}) {
            const auto* r = find(snaps, name);
            if (r == nullptr || !r->replica.replay_lsn.is_ok() ||
                *r->replica.replay_lsn.value() < target) {
                return false;
            }
        }
        return true;
    });
    ASSERT_TRUE(caught_up.has_value()) << "replicas did not replay the workload within 15 s";
    std::cout << "[ measured ] replicas replayed " << kTransactions << " commits within "
              << caught_up->count() << " ms of the post-workload snapshot\n";
}

TEST(LabClusterTest, StoppedReplicaIsDetectedAsDownAndRecovers) {
    REQUIRE_LAB();
    const Lab lab = Lab::load();
    auto collectors = lab.collectors();
    ASSERT_TRUE(cluster_healthy(lab.collect_all(collectors))) << "cluster not healthy at start";

    ComposeGuard guard("start pg-replica-2");
    const auto stop_started = SteadyClock::now();
    ASSERT_EQ(compose("stop pg-replica-2"), 0);
    const auto stop_ms =
        std::chrono::duration_cast<milliseconds>(SteadyClock::now() - stop_started);

    const auto detected = wait_for(lab, collectors, seconds{20}, [](const auto& snaps) {
        const auto t = derive_topology(snaps);
        const auto* r2 = find(snaps, "pg-replica-2");
        const auto* p = find(snaps, "pg-primary");
        return r2 != nullptr && r2->availability == Availability::kDown && p != nullptr &&
               p->standby_count.is_ok() && *p->standby_count.value() == 1.0 &&
               has_issue(t, TopologyIssueCode::kNodeDown, "pg-replica-2") &&
               has_issue(t, TopologyIssueCode::kSlotInactive, "pg_replica_2");
    });
    ASSERT_TRUE(detected.has_value()) << "outage not detected within 20 s";

    const auto snaps = lab.collect_all(collectors);
    const auto* r2 = find(snaps, "pg-replica-2");
    ASSERT_NE(r2, nullptr);
    EXPECT_EQ(r2->errors.front().kind, SqlErrorKind::kConnectionFailed);
    // Missing, not zero: nothing about the stopped replica is reported as a value.
    EXPECT_EQ(r2->replica.replay_lag_seconds.quality(), Quality::kMissing);
    EXPECT_EQ(r2->replica.apply_backlog_bytes.quality(), Quality::kMissing);
    EXPECT_EQ(node_of(snaps, "pg-replica-1").availability, Availability::kUp);

    const auto restart_started = SteadyClock::now();
    ASSERT_EQ(compose("start pg-replica-2"), 0);
    const auto recovered = wait_for(lab, collectors, seconds{60}, cluster_healthy);
    ASSERT_TRUE(recovered.has_value()) << "replica did not resume streaming within 60 s";
    const auto recovery_ms =
        std::chrono::duration_cast<milliseconds>(SteadyClock::now() - restart_started);
    std::cout << "[ measured ] docker compose stop took " << stop_ms.count()
              << " ms; outage detected " << detected->count()
              << " ms after stop returned; healthy again " << recovery_ms.count()
              << " ms after start was issued\n";
}

TEST(LabClusterTest, PausedReplicaTimesOutInsteadOfHanging) {
    REQUIRE_LAB();
    const Lab lab = Lab::load();
    ConnectOptions options;
    options.query_timeout = milliseconds{1000};
    options.statement_timeout = milliseconds{750};
    auto collectors = lab.collectors(options);
    ASSERT_TRUE(cluster_healthy(lab.collect_all(collectors))) << "cluster not healthy at start";

    ComposeGuard guard("unpause pg-replica-1");
    ASSERT_EQ(compose("pause pg-replica-1"), 0);

    // First cycle: the open connection stops answering; the client deadline fires.
    auto start = SteadyClock::now();
    auto snaps = lab.collect_all(collectors);
    const auto first_ms = std::chrono::duration_cast<milliseconds>(SteadyClock::now() - start);
    const auto* r1 = find(snaps, "pg-replica-1");
    ASSERT_NE(r1, nullptr);
    EXPECT_EQ(r1->availability, Availability::kDown);
    ASSERT_FALSE(r1->errors.empty());
    EXPECT_EQ(r1->errors.front().kind, SqlErrorKind::kTimeout);
    EXPECT_LT(first_ms.count(), 5000);

    // Second cycle: reconnecting to a frozen server fails by connect_timeout, not forever.
    start = SteadyClock::now();
    snaps = lab.collect_all(collectors);
    const auto second_ms = std::chrono::duration_cast<milliseconds>(SteadyClock::now() - start);
    EXPECT_EQ(node_of(snaps, "pg-replica-1").availability, Availability::kDown);
    EXPECT_LT(second_ms.count(), 8000);
    EXPECT_EQ(node_of(snaps, "pg-primary").availability, Availability::kUp);

    ASSERT_EQ(compose("unpause pg-replica-1"), 0);
    const auto recovered = wait_for(lab, collectors, seconds{30}, cluster_healthy);
    ASSERT_TRUE(recovered.has_value()) << "cluster not healthy 30 s after unpause";
    std::cout << "[ measured ] paused replica: cycle with open connection " << first_ms.count()
              << " ms (client deadline 1000 ms), reconnect cycle " << second_ms.count()
              << " ms (connect_timeout 2 s), healthy again " << recovered->count()
              << " ms after unpause\n";
}

}  // namespace
}  // namespace faulttwin::collectors
