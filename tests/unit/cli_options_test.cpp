#include <gtest/gtest.h>

#include <string_view>
#include <vector>

#include "options.hpp"

namespace faulttwin::cli {
namespace {

ParseResult parse(std::vector<std::string_view> args) {
    return parse_options(args);
}

const Options& as_options(const ParseResult& result) {
    return std::get<Options>(result);
}

bool is_usage_error(const ParseResult& result) {
    return std::holds_alternative<UsageError>(result);
}

TEST(CliOptionsTest, DefaultsWithNoArguments) {
    const auto result = parse({});
    ASSERT_TRUE(std::holds_alternative<Options>(result));
    const auto& options = as_options(result);
    EXPECT_EQ(options.config.node_count, 3U);
    EXPECT_EQ(options.config.samples_per_node, 5U);
    EXPECT_EQ(options.config.seed, 42U);
    EXPECT_EQ(options.format, OutputFormat::kText);
    EXPECT_FALSE(options.quiet);
}

TEST(CliOptionsTest, ParsesEveryOption) {
    const auto result =
        parse({"--nodes", "8", "--samples-per-node", "100", "--seed", "18446744073709551615",
               "--invalid-fraction", "0.25", "--lag-missing-fraction", "0", "--start-unix-ms",
               "1700000000000", "--interval-ms", "500", "--format", "jsonl", "--quiet"});
    ASSERT_TRUE(std::holds_alternative<Options>(result));
    const auto& o = as_options(result);
    EXPECT_EQ(o.config.node_count, 8U);
    EXPECT_EQ(o.config.samples_per_node, 100U);
    EXPECT_EQ(o.config.seed, 18446744073709551615ULL);
    EXPECT_DOUBLE_EQ(o.config.invalid_fraction, 0.25);
    EXPECT_DOUBLE_EQ(o.config.replica_lag_missing_fraction, 0.0);
    EXPECT_EQ(o.config.start_time.time_since_epoch(), std::chrono::milliseconds{1700000000000});
    EXPECT_EQ(o.config.interval, std::chrono::milliseconds{500});
    EXPECT_EQ(o.format, OutputFormat::kJsonLines);
    EXPECT_TRUE(o.quiet);
}

TEST(CliOptionsTest, HelpWinsImmediately) {
    EXPECT_TRUE(std::holds_alternative<HelpRequested>(parse({"--help"})));
    EXPECT_TRUE(std::holds_alternative<HelpRequested>(parse({"--nodes", "2", "-h"})));
}

TEST(CliOptionsTest, RejectsMalformedAndOutOfRangeValues) {
    const std::vector<std::vector<std::string_view>> bad{
        {"--nodes", "0"},
        {"--nodes", "65"},
        {"--nodes", "-1"},
        {"--nodes", "3x"},
        {"--nodes", ""},
        {"--samples-per-node", "100001"},
        {"--seed", "18446744073709551616"},  // overflows uint64
        {"--invalid-fraction", "1.0001"},
        {"--invalid-fraction", "-0.1"},
        {"--invalid-fraction", "nan"},
        {"--invalid-fraction", "inf"},
        {"--invalid-fraction", "0.5abc"},
        {"--start-unix-ms", "0"},
        {"--interval-ms", "0"},
        {"--format", "xml"},
        {"--nodes"},  // missing value
        {"--unknown"},
    };
    for (const auto& args : bad) {
        EXPECT_TRUE(is_usage_error(parse(args)))
            << args.front() << " " << (args.size() > 1 ? args[1] : "");
    }
}

TEST(CliOptionsTest, UsageMentionsProgramName) {
    EXPECT_NE(usage_text("faulttwin-synth").find("faulttwin-synth"), std::string::npos);
}

}  // namespace
}  // namespace faulttwin::cli
