#include "detect_options.hpp"

#include <gtest/gtest.h>

#include <initializer_list>
#include <string_view>
#include <variant>
#include <vector>

namespace {

namespace dt = faulttwin::detect;

dt::DetectCommand parse(std::initializer_list<std::string_view> args) {
    std::vector<std::string_view> argv;
    argv.reserve(args.size() + 1);
    argv.emplace_back("faulttwin-detect");
    for (const auto arg : args) {
        argv.push_back(arg);
    }
    return dt::parse_detect_options(argv);
}

TEST(DetectOptionsTest, EvaluateDefaultsAreDisjointSplits) {
    const auto c = parse({"evaluate"});
    const auto& o = std::get<dt::EvaluateOptions>(c);
    EXPECT_EQ(o.validation.first, 1U);
    EXPECT_EQ(o.validation.last, 40U);
    EXPECT_EQ(o.test.first, 1001U);
    ASSERT_TRUE(o.shifted);
    EXPECT_EQ(o.shifted->first, 2001U);
    EXPECT_EQ(o.grace_seconds, 30U);
}

TEST(DetectOptionsTest, ParsesRangesAndOptions) {
    const auto c = parse({"evaluate", "--validation-seeds", "5-9", "--test-seeds", "100",
                          "--shifted-seeds", "none", "--grace", "10", "--out", "x"});
    const auto& o = std::get<dt::EvaluateOptions>(c);
    EXPECT_EQ(o.validation.size(), 5U);
    EXPECT_EQ(o.test.first, 100U);
    EXPECT_EQ(o.test.last, 100U);
    EXPECT_FALSE(o.shifted);
    EXPECT_EQ(o.grace_seconds, 10U);
    EXPECT_EQ(o.out_dir, "x");

    const auto s = parse({"scenario", "--seed", "7", "--format", "jsonl"});
    EXPECT_EQ(std::get<dt::ScenarioOptions>(s).seed, 7U);
    EXPECT_TRUE(std::get<dt::ScenarioOptions>(s).jsonl);
}

TEST(DetectOptionsTest, RefusesOverlappingSplitsAndBadInput) {
    EXPECT_TRUE(std::holds_alternative<dt::UsageError>(
        parse({"evaluate", "--validation-seeds", "1-10", "--test-seeds", "10-20"})));
    EXPECT_TRUE(
        std::holds_alternative<dt::UsageError>(parse({"evaluate", "--shifted-seeds", "30-1001"})));
    EXPECT_TRUE(std::holds_alternative<dt::UsageError>(parse({"evaluate", "--grace", "-1"})));
    EXPECT_TRUE(std::holds_alternative<dt::UsageError>(
        parse({"evaluate", "--validation-seeds", "7-7"})));  // learned detectors need 2 halves
    EXPECT_TRUE(std::holds_alternative<dt::UsageError>(parse({"evaluate", "--test-seeds"})));
    EXPECT_TRUE(std::holds_alternative<dt::UsageError>(parse({"evaluate", "--bogus", "1"})));
    EXPECT_TRUE(std::holds_alternative<dt::UsageError>(parse({"scenario", "--seed", "x"})));
    EXPECT_TRUE(std::holds_alternative<dt::UsageError>(parse({"train"})));
    EXPECT_TRUE(std::holds_alternative<dt::UsageError>(parse({})));
    EXPECT_TRUE(std::holds_alternative<dt::HelpRequested>(parse({"--help"})));
    EXPECT_FALSE(dt::parse_seed_range("9-3"));
    EXPECT_FALSE(dt::parse_seed_range("1-"));
}

}  // namespace
