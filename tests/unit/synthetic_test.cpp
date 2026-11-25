#include "faulttwin/telemetry/synthetic.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <set>

namespace faulttwin::telemetry {
namespace {

bool same_double(double a, double b) {
    return (std::isnan(a) && std::isnan(b)) || a == b;
}

bool same_raw(const RawTelemetrySample& a, const RawTelemetrySample& b) {
    const bool lag_equal = a.replication_lag_ms.has_value() == b.replication_lag_ms.has_value() &&
                           (!a.replication_lag_ms.has_value() ||
                            same_double(*a.replication_lag_ms, *b.replication_lag_ms));
    return a.node_id == b.node_id && a.role == b.role && a.collected_at == b.collected_at &&
           same_double(a.cpu_usage_percent, b.cpu_usage_percent) &&
           same_double(a.memory_usage_percent, b.memory_usage_percent) &&
           same_double(a.disk_usage_percent, b.disk_usage_percent) && lag_equal;
}

TEST(SyntheticTest, ProducesNodeCountTimesSamples) {
    SyntheticConfig config;
    config.node_count = 4;
    config.samples_per_node = 7;
    EXPECT_EQ(generate_synthetic_samples(config).size(), 28U);
}

TEST(SyntheticTest, SameSeedSameSequence) {
    SyntheticConfig config;
    config.node_count = 3;
    config.samples_per_node = 200;
    config.invalid_fraction = 0.3;
    const auto a = generate_synthetic_samples(config);
    const auto b = generate_synthetic_samples(config);
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_TRUE(same_raw(a[i].raw, b[i].raw)) << "index " << i;
        EXPECT_EQ(a[i].defect, b[i].defect) << "index " << i;
    }
}

TEST(SyntheticTest, DifferentSeedDifferentSequence) {
    SyntheticConfig config;
    config.samples_per_node = 50;
    SyntheticConfig other = config;
    other.seed = config.seed + 1;
    const auto a = generate_synthetic_samples(config);
    const auto b = generate_synthetic_samples(other);
    std::size_t differing = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        differing += same_raw(a[i].raw, b[i].raw) ? 0U : 1U;
    }
    EXPECT_GT(differing, a.size() / 2);
}

// Golden values pin the PRNG and value mapping. If this fails after an
// intentional change to the generator, update the values and say so in review.
TEST(SyntheticTest, GoldenFirstSample) {
    SyntheticConfig config;
    config.seed = 42;
    config.node_count = 1;
    config.samples_per_node = 1;
    const auto samples = generate_synthetic_samples(config);
    ASSERT_EQ(samples.size(), 1U);
    const auto& raw = samples.front().raw;
    EXPECT_EQ(raw.node_id, "pg-primary");
    EXPECT_EQ(raw.role, NodeRole::kPrimary);
    // Independently computed with a Python SplitMix64 reference (see
    // docs/phase-reports/phase-01.md).
    EXPECT_DOUBLE_EQ(raw.cpu_usage_percent, 64.32519030174586);
    EXPECT_FALSE(raw.replication_lag_ms.has_value());
}

TEST(SyntheticTest, CleanRunIsEntirelyValid) {
    SyntheticConfig config;
    config.node_count = 5;
    config.samples_per_node = 500;
    config.invalid_fraction = 0.0;
    const Timestamp now = synthetic_reference_now(config);
    for (const auto& sample : generate_synthetic_samples(config)) {
        EXPECT_FALSE(sample.defect.has_value());
        EXPECT_TRUE(validate_sample(sample.raw, now).ok());
    }
}

TEST(SyntheticTest, EveryDefectIsDetectedAsExactlyItsExpectedIssue) {
    SyntheticConfig config;
    config.node_count = 3;
    config.samples_per_node = 2000;
    config.invalid_fraction = 1.0;
    const Timestamp now = synthetic_reference_now(config);
    std::set<InjectedDefect> seen;
    for (const auto& sample : generate_synthetic_samples(config)) {
        ASSERT_TRUE(sample.defect.has_value());
        seen.insert(*sample.defect);
        const ValidationResult result = validate_sample(sample.raw, now);
        ASSERT_FALSE(result.ok()) << to_string(*sample.defect);
        ASSERT_EQ(result.issues.size(), 1U) << to_string(*sample.defect);
        EXPECT_EQ(result.issues.front(), expected_issue(*sample.defect))
            << to_string(*sample.defect);
    }
    EXPECT_EQ(seen.size(), kInjectedDefectCount) << "every defect kind should occur";
}

TEST(SyntheticTest, ReplicasDistinguishMissingLagFromMeasuredLag) {
    SyntheticConfig config;
    config.node_count = 4;
    config.samples_per_node = 500;
    config.replica_lag_missing_fraction = 0.2;
    std::size_t missing = 0;
    std::size_t measured = 0;
    for (const auto& sample : generate_synthetic_samples(config)) {
        if (sample.raw.role == NodeRole::kReplica) {
            (sample.raw.replication_lag_ms.has_value() ? measured : missing) += 1;
        } else {
            EXPECT_FALSE(sample.raw.replication_lag_ms.has_value());
        }
    }
    EXPECT_GT(missing, 0U);
    EXPECT_GT(measured, missing);
}

TEST(SyntheticTest, ReferenceNowIsLastTick) {
    SyntheticConfig config;
    config.samples_per_node = 10;
    config.interval = std::chrono::milliseconds{250};
    EXPECT_EQ(synthetic_reference_now(config), config.start_time + std::chrono::milliseconds{2250});
}

}  // namespace
}  // namespace faulttwin::telemetry
