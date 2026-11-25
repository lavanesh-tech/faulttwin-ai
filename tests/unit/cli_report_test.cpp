#include <gtest/gtest.h>

#include <sstream>

#include "report.hpp"

namespace faulttwin::cli {
namespace {

using telemetry::InjectedDefect;
using telemetry::SyntheticSample;

TEST(CliReportTest, FormatsTimestampAsUtcWithMilliseconds) {
    const telemetry::Timestamp t{std::chrono::milliseconds{1767225600123}};
    EXPECT_EQ(format_timestamp_utc(t), "2026-01-01T00:00:00.123Z");
    EXPECT_EQ(format_timestamp_utc(telemetry::Timestamp{}), "1970-01-01T00:00:00.000Z");
}

TEST(CliReportTest, JsonQuoteEscapesUntrustedText) {
    EXPECT_EQ(json_quote("plain"), "\"plain\"");
    EXPECT_EQ(json_quote("a\"b\\c"), "\"a\\\"b\\\\c\"");
    EXPECT_EQ(json_quote("x\ny\t"), "\"x\\ny\\t\"");
    EXPECT_EQ(json_quote(std::string_view{"\x01\x1b", 2}), "\"\\u0001\\u001b\"");
}

TEST(CliReportTest, GroundTruthMatching) {
    telemetry::SyntheticConfig config;
    config.node_count = 2;
    config.samples_per_node = 1;
    const auto now = telemetry::synthetic_reference_now(config);
    auto samples = telemetry::generate_synthetic_samples(config);
    ASSERT_EQ(samples.size(), 2U);

    SyntheticSample clean = samples[0];
    EXPECT_TRUE(matches_ground_truth(clean, telemetry::validate_sample(clean.raw, now)));

    // Claiming a defect that is not present must count as a mismatch.
    SyntheticSample mislabelled = clean;
    mislabelled.defect = InjectedDefect::kCpuOutOfRange;
    EXPECT_FALSE(
        matches_ground_truth(mislabelled, telemetry::validate_sample(mislabelled.raw, now)));

    // A defect that is present and correctly labelled matches.
    SyntheticSample defective = clean;
    defective.raw.cpu_usage_percent = 150.0;
    defective.defect = InjectedDefect::kCpuOutOfRange;
    EXPECT_TRUE(matches_ground_truth(defective, telemetry::validate_sample(defective.raw, now)));
}

TEST(CliReportTest, SummaryCountsAndJsonLineShape) {
    telemetry::SyntheticConfig config;
    config.node_count = 3;
    config.samples_per_node = 20;
    config.invalid_fraction = 0.5;
    const auto now = telemetry::synthetic_reference_now(config);

    RunSummary summary;
    std::ostringstream lines;
    for (const auto& sample : telemetry::generate_synthetic_samples(config)) {
        const auto result = telemetry::validate_sample(sample.raw, now);
        accumulate(summary, sample, result);
        write_sample_line(lines, OutputFormat::kJsonLines, sample, result);
    }
    EXPECT_EQ(summary.attempted, 60U);
    EXPECT_EQ(summary.valid + summary.invalid, summary.attempted);
    EXPECT_EQ(summary.invalid, summary.injected_defects);
    EXPECT_EQ(summary.ground_truth_mismatches, 0U);

    std::istringstream in{lines.str()};
    std::string line;
    std::size_t count = 0;
    while (std::getline(in, line)) {
        ++count;
        EXPECT_EQ(line.front(), '{');
        EXPECT_EQ(line.back(), '}');
        EXPECT_NE(line.find("\"matches_ground_truth\":true"), std::string::npos) << line;
    }
    EXPECT_EQ(count, 60U);
}

}  // namespace
}  // namespace faulttwin::cli
