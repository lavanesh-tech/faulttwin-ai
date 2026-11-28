#include "daemon_options.hpp"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "monitor_loop.hpp"

namespace faulttwin::server {
namespace {

using Env = std::map<std::string, std::string, std::less<>>;

EnvLookup lookup(const Env& env) {
    return [&env](std::string_view name) -> std::optional<std::string> {
        const auto it = env.find(name);
        return it != env.end() ? std::optional<std::string>(it->second) : std::nullopt;
    };
}

std::variant<DaemonOptions, HelpRequested, UsageError> parse(std::vector<std::string_view> args,
                                                             const Env& env = {}) {
    return parse_daemon_options(args, lookup(env));
}

std::string usage_error(const std::variant<DaemonOptions, HelpRequested, UsageError>& r) {
    return std::holds_alternative<UsageError>(r) ? std::get<UsageError>(r).message : "";
}

TEST(DaemonOptions, DefaultsAreLoopbackSyntheticAndMemory) {
    const auto r = parse({});
    ASSERT_TRUE(std::holds_alternative<DaemonOptions>(r)) << usage_error(r);
    const auto& o = std::get<DaemonOptions>(r);
    EXPECT_EQ(o.listen_host, "127.0.0.1");
    EXPECT_EQ(o.listen_port, 8080);
    EXPECT_EQ(o.source, SourceKind::kSynthetic);
    EXPECT_EQ(o.store, StoreKind::kMemory);
    EXPECT_TRUE(o.api_key.empty());
}

TEST(DaemonOptions, HelpIsRecognised) {
    EXPECT_TRUE(std::holds_alternative<HelpRequested>(parse({"--help"})));
    EXPECT_TRUE(std::holds_alternative<HelpRequested>(parse({"-h"})));
}

TEST(DaemonOptions, RejectsBadValues) {
    EXPECT_FALSE(usage_error(parse({"--listen", "nohostport"})).empty());
    EXPECT_FALSE(usage_error(parse({"--listen", "127.0.0.1:70000"})).empty());
    EXPECT_FALSE(usage_error(parse({"--source", "kafka"})).empty());
    EXPECT_FALSE(usage_error(parse({"--store", "sqlite"})).empty());
    EXPECT_FALSE(usage_error(parse({"--interval-ms", "0"})).empty());
    EXPECT_FALSE(usage_error(parse({"--http-threads", "65"})).empty());
    EXPECT_FALSE(usage_error(parse({"--log-level", "trace"})).empty());
    EXPECT_FALSE(usage_error(parse({"--interval-ms"})).empty());
    EXPECT_FALSE(usage_error(parse({"--bogus"})).empty());
}

TEST(DaemonOptions, NonLoopbackListenNeedsAllowRemote) {
    EXPECT_NE(usage_error(parse({"--listen", "0.0.0.0:8080"})).find("--allow-remote"),
              std::string::npos);
    const auto r = parse({"--listen", "0.0.0.0:8080", "--allow-remote"});
    ASSERT_TRUE(std::holds_alternative<DaemonOptions>(r)) << usage_error(r);
    EXPECT_EQ(std::get<DaemonOptions>(r).listen_host, "0.0.0.0");
}

TEST(DaemonOptions, ApiKeyComesFromTheEnvironmentAndMustBeLongEnough) {
    EXPECT_FALSE(usage_error(parse({}, {{"FT_API_KEY", "short"}})).empty());
    const auto r = parse({}, {{"FT_API_KEY", "0123456789abcdef"}});
    ASSERT_TRUE(std::holds_alternative<DaemonOptions>(r)) << usage_error(r);
    EXPECT_EQ(std::get<DaemonOptions>(r).api_key, "0123456789abcdef");
}

TEST(DaemonOptions, PostgresSourceAndStoreNeedTheirSecrets) {
    EXPECT_NE(usage_error(parse({"--source", "postgres"})).find("FT_MONITOR_NODES"),
              std::string::npos);
    EXPECT_NE(usage_error(parse({"--store", "postgres"})).find("FT_METADB_PASSWORD"),
              std::string::npos);
    const Env env{{"FT_MONITOR_NODES", "pg-primary=127.0.0.1:55432"},
                  {"FT_MONITOR_PASSWORD", "monitor-secret"},
                  {"FT_METADB_PASSWORD", "metadb-secret"},
                  {"FT_METADB_PORT", "55999"}};
    const auto r = parse({"--source", "postgres", "--store", "postgres"}, env);
    ASSERT_TRUE(std::holds_alternative<DaemonOptions>(r)) << usage_error(r);
    const auto& o = std::get<DaemonOptions>(r);
    ASSERT_EQ(o.nodes.size(), 1U);
    EXPECT_EQ(o.nodes[0].node, "pg-primary");
    EXPECT_EQ(o.monitor_credentials.password, "monitor-secret");
    EXPECT_EQ(o.metadb.credentials.password, "metadb-secret");
    EXPECT_EQ(o.metadb.endpoint.port, 55999);
}

TEST(DaemonOptions, UsageNamesEverySecretVariable) {
    const auto text = daemon_usage("faulttwind");
    for (const char* name : {"FT_API_KEY", "FT_MONITOR_PASSWORD", "FT_METADB_PASSWORD"}) {
        EXPECT_NE(text.find(name), std::string::npos) << name;
    }
}

TEST(NodeStatus, DerivedFromOneCyclesObservations) {
    using features::MetricId;
    using features::Observation;
    const core::WallTime t{std::chrono::sys_days{std::chrono::year{2026} / 1 / 1}};
    const std::vector<Observation> obs{
        Observation::ok("pg-primary", MetricId::kAvailability, t, 1.0),
        Observation::ok("pg-primary", MetricId::kProbeLatencyMs, t, 0.5),
        Observation::ok("pg-replica-1", MetricId::kAvailability, t, 1.0),
        Observation::ok("pg-replica-1", MetricId::kReplicationLagSeconds, t, 2.5),
        Observation::missing("pg-replica-2", MetricId::kAvailability, t),
    };
    const auto status = node_status_from_observations(obs, t);
    ASSERT_EQ(status.size(), 3U);
    EXPECT_EQ(status[0].node, "pg-primary");
    EXPECT_EQ(status[0].role, "primary");
    EXPECT_EQ(status[0].availability, "up");
    EXPECT_EQ(status[0].probe_latency_ms, 0.5);
    EXPECT_FALSE(status[0].replication_lag_seconds.has_value());
    EXPECT_EQ(status[1].role, "replica");
    EXPECT_EQ(status[1].replication_lag_seconds, 2.5);
    EXPECT_EQ(status[2].availability, "down");
}

}  // namespace
}  // namespace faulttwin::server
