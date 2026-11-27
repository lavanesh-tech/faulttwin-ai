#include <gtest/gtest.h>

#include <map>
#include <sstream>
#include <string>
#include <variant>

#include "monitor_options.hpp"
#include "monitor_report.hpp"

namespace faulttwin::monitor {
namespace {

EnvLookup env_of(std::map<std::string, std::string> vars) {
    return [vars = std::move(vars)](std::string_view name) -> std::optional<std::string> {
        const auto it = vars.find(std::string(name));
        return it == vars.end() ? std::nullopt : std::optional<std::string>(it->second);
    };
}

const std::map<std::string, std::string> kEnv{
    {"FT_MONITOR_NODES", "pg-primary=127.0.0.1:55432,pg-replica-1=127.0.0.1:55433"},
    {"FT_MONITOR_PASSWORD", "secret"}};

TEST(MonitorOptionsTest, DefaultsAndEnvironment) {
    const auto parsed = parse_monitor_options({}, env_of(kEnv));
    const auto* o = std::get_if<MonitorOptions>(&parsed);
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->nodes.size(), 2U);
    EXPECT_EQ(o->credentials.user, "ft_monitor");
    EXPECT_EQ(o->credentials.password, "secret");
    EXPECT_EQ(o->credentials.dbname, "postgres");
    EXPECT_EQ(o->cycles, 1U);
    EXPECT_EQ(o->format, OutputFormat::kText);
}

TEST(MonitorOptionsTest, ParsesOptions) {
    const auto parsed =
        parse_monitor_options({"--cycles", "5", "--interval-ms", "250", "--format", "json",
                               "--query-timeout-ms", "800", "--no-resources", "--detect"},
                              env_of(kEnv));
    const auto* o = std::get_if<MonitorOptions>(&parsed);
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->cycles, 5U);
    EXPECT_EQ(o->interval.count(), 250);
    EXPECT_EQ(o->format, OutputFormat::kJson);
    EXPECT_EQ(o->connect.query_timeout.count(), 800);
    EXPECT_EQ(o->connect.statement_timeout.count(), 600);
    EXPECT_FALSE(o->resources);
    EXPECT_TRUE(o->detect);
}

TEST(MonitorOptionsTest, RejectsBadInput) {
    for (const std::vector<std::string_view>& args :
         {std::vector<std::string_view>{"--cycles", "0"},
          {"--cycles"},
          {"--format", "xml"},
          {"--connect-timeout-s", "1"},
          {"--password", "x"},
          {"--interval-ms", "-1"}}) {
        EXPECT_TRUE(std::holds_alternative<UsageError>(parse_monitor_options(args, env_of(kEnv))))
            << args.front();
    }
}

TEST(MonitorOptionsTest, RequiresNodesAndPasswordFromEnvironment) {
    EXPECT_TRUE(std::holds_alternative<UsageError>(
        parse_monitor_options({}, env_of({{"FT_MONITOR_PASSWORD", "x"}}))));
    EXPECT_TRUE(std::holds_alternative<UsageError>(
        parse_monitor_options({}, env_of({{"FT_MONITOR_NODES", "a=h:1"}}))));
    EXPECT_TRUE(std::holds_alternative<UsageError>(parse_monitor_options(
        {}, env_of({{"FT_MONITOR_NODES", "a=h:1"}, {"FT_MONITOR_PASSWORD", ""}}))));
    EXPECT_TRUE(
        std::holds_alternative<HelpRequested>(parse_monitor_options({"--help"}, env_of({}))));
}

TEST(MonitorReportTest, JsonEscape) {
    EXPECT_EQ(json_escape("a\"b\\c\nd\te"), "a\\\"b\\\\c\\nd\\te");
    EXPECT_EQ(json_escape(std::string("\x01", 1)), "\\u0001");
    EXPECT_EQ(json_escape("ünïcode"), "ünïcode");
}

TEST(MonitorReportTest, NearestRankPercentile) {
    EXPECT_FALSE(percentile({}, 50).has_value());
    EXPECT_EQ(percentile({5}, 99), 5);
    EXPECT_EQ(percentile({4, 1, 3, 2}, 50), 2);
    EXPECT_EQ(percentile({4, 1, 3, 2}, 100), 4);
    EXPECT_EQ(percentile({4, 1, 3, 2}, 0), 1);
}

TEST(MonitorReportTest, SummaryCountsSuccessAndCoverage) {
    RunSummary summary;
    CycleRecord record;
    record.nodes.resize(2);
    record.nodes[0].availability = collectors::Availability::kUp;
    record.nodes[0].probe_latency_ms = collectors::Reading<double>::ok(1.0);
    record.nodes[1].availability = collectors::Availability::kDown;
    record.node_wall_ms = {1.0, 2.0};
    record.freshness_ms = {0.5, 0.5};
    accumulate(summary, record);
    EXPECT_EQ(summary.attempts, 2U);
    EXPECT_EQ(summary.successes, 1U);
    EXPECT_EQ(summary.readings_ok, 1U);
    std::ostringstream out;
    write_summary_json(out, summary);
    EXPECT_NE(out.str().find("\"collection_success_percent\":50"), std::string::npos);
}

}  // namespace
}  // namespace faulttwin::monitor
