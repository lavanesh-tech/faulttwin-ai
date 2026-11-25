#include "faulttwin/telemetry/validation.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <limits>
#include <ostream>
#include <string>
#include <vector>

namespace faulttwin::telemetry {
namespace {

using namespace std::chrono_literals;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

// A fixed reference time keeps every test independent of the wall clock.
const Timestamp kNow = Timestamp{std::chrono::sys_days{std::chrono::year{2026} / 3 / 1}};

RawTelemetrySample valid_replica() {
    RawTelemetrySample raw;
    raw.node_id = "pg-replica-1";
    raw.role = NodeRole::kReplica;
    raw.collected_at = kNow - 1s;
    raw.cpu_usage_percent = 42.0;
    raw.memory_usage_percent = 55.5;
    raw.disk_usage_percent = 12.25;
    raw.replication_lag_ms = 120.0;
    return raw;
}

RawTelemetrySample valid_primary() {
    RawTelemetrySample raw = valid_replica();
    raw.node_id = "pg-primary";
    raw.role = NodeRole::kPrimary;
    raw.replication_lag_ms.reset();
    return raw;
}

std::vector<ValidationIssue> issues_of(const RawTelemetrySample& raw) {
    return validate_sample(raw, kNow).issues;
}

// ---- Percentages --------------------------------------------------------------

struct PercentCase {
    const char* name;  // used as the gtest parameter name
    double value;
    std::optional<ValidationCode> expected;
};

// Keeps test names and failure messages readable instead of a raw byte dump.
void PrintTo(const PercentCase& c, std::ostream* os) {
    *os << c.name;
}

class PercentageCheckTest : public ::testing::TestWithParam<PercentCase> {};

TEST_P(PercentageCheckTest, ClassifiesValue) {
    EXPECT_EQ(check_percentage(GetParam().value), GetParam().expected);
}

INSTANTIATE_TEST_SUITE_P(
    Boundaries, PercentageCheckTest,
    ::testing::Values(
        PercentCase{"Zero", 0.0, std::nullopt}, PercentCase{"NegativeZero", -0.0, std::nullopt},
        PercentCase{"Hundred", 100.0, std::nullopt}, PercentCase{"Fifty", 50.0, std::nullopt},
        PercentCase{"JustBelowZero", std::nextafter(0.0, -1.0), ValidationCode::kOutOfRange},
        PercentCase{"JustAboveHundred", std::nextafter(100.0, 200.0), ValidationCode::kOutOfRange},
        PercentCase{"MinusFive", -5.0, ValidationCode::kOutOfRange},
        PercentCase{"NaN", kNaN, ValidationCode::kNotFinite},
        PercentCase{"PlusInfinity", kInf, ValidationCode::kNotFinite},
        PercentCase{"MinusInfinity", -kInf, ValidationCode::kNotFinite}),
    [](const ::testing::TestParamInfo<PercentCase>& param_info) {
        return std::string{param_info.param.name};
    });

TEST(ValidationTest, EachPercentageFieldIsReportedSeparately) {
    RawTelemetrySample raw = valid_replica();
    raw.cpu_usage_percent = 101.0;
    raw.memory_usage_percent = kNaN;
    raw.disk_usage_percent = -kInf;
    const std::vector<ValidationIssue> expected{
        {Field::kCpuUsagePercent, ValidationCode::kOutOfRange},
        {Field::kMemoryUsagePercent, ValidationCode::kNotFinite},
        {Field::kDiskUsagePercent, ValidationCode::kNotFinite},
    };
    EXPECT_EQ(issues_of(raw), expected);
}

// ---- Timestamps ----------------------------------------------------------------

TEST(ValidationTest, TimestampPolicy) {
    const ValidationPolicy policy{};  // 5 s future skew
    EXPECT_EQ(check_timestamp(kNow, kNow, policy), std::nullopt);
    EXPECT_EQ(check_timestamp(kNow - 24h, kNow, policy), std::nullopt)
        << "old is stale, not invalid";
    EXPECT_EQ(check_timestamp(kNow + 5s, kNow, policy), std::nullopt) << "skew bound is inclusive";
    EXPECT_EQ(check_timestamp(kNow + 5s + 1ms, kNow, policy), ValidationCode::kTimestampInFuture);
    EXPECT_EQ(check_timestamp(Timestamp{}, kNow, policy), ValidationCode::kTimestampUnset);
    EXPECT_EQ(check_timestamp(Timestamp{} - 1s, kNow, policy), ValidationCode::kTimestampUnset);
}

TEST(ValidationTest, FutureSkewIsConfigurable) {
    const ValidationPolicy strict{0ms};
    EXPECT_EQ(check_timestamp(kNow + 1ms, kNow, strict), ValidationCode::kTimestampInFuture);
    const ValidationPolicy lenient{1min};
    EXPECT_EQ(check_timestamp(kNow + 30s, kNow, lenient), std::nullopt);
}

// ---- Replication lag semantics ----------------------------------------------------

TEST(ValidationTest, MissingLagOnReplicaIsNotZeroLag) {
    RawTelemetrySample raw = valid_replica();
    raw.replication_lag_ms.reset();
    const ValidationResult result = validate_sample(raw, kNow);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.sample->replication_lag_state(), ReplicationLagState::kNotMeasured);
    EXPECT_FALSE(result.sample->replication_lag_ms().has_value());
}

TEST(ValidationTest, ZeroLagOnReplicaIsAMeasurement) {
    RawTelemetrySample raw = valid_replica();
    raw.replication_lag_ms = 0.0;
    const ValidationResult result = validate_sample(raw, kNow);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.sample->replication_lag_state(), ReplicationLagState::kMeasured);
    EXPECT_EQ(result.sample->replication_lag_ms(), 0.0);
}

TEST(ValidationTest, ReplicationLagRules) {
    EXPECT_EQ(check_replication_lag(NodeRole::kReplica, 0.0), std::nullopt);
    EXPECT_EQ(check_replication_lag(NodeRole::kReplica, std::nullopt), std::nullopt);
    EXPECT_EQ(check_replication_lag(NodeRole::kReplica, -0.001), ValidationCode::kNegative);
    EXPECT_EQ(check_replication_lag(NodeRole::kReplica, kNaN), ValidationCode::kNotFinite);
    EXPECT_EQ(check_replication_lag(NodeRole::kReplica, kInf), ValidationCode::kNotFinite);
    EXPECT_EQ(check_replication_lag(NodeRole::kPrimary, std::nullopt), std::nullopt);
    EXPECT_EQ(check_replication_lag(NodeRole::kPrimary, 0.0),
              ValidationCode::kReplicationLagNotApplicable)
        << "a primary reporting even zero lag is a semantic error";
    EXPECT_EQ(check_replication_lag(NodeRole::kUnknown, 10.0),
              ValidationCode::kReplicationLagNotApplicable);
}

TEST(ValidationTest, InvalidRoleStillChecksLagNumerically) {
    const auto corrupt = static_cast<NodeRole>(std::uint8_t{9});
    EXPECT_EQ(check_replication_lag(corrupt, 10.0), std::nullopt);
    EXPECT_EQ(check_replication_lag(corrupt, -1.0), ValidationCode::kNegative);
}

TEST(ValidationTest, PrimaryLagStateIsNotApplicable) {
    const ValidationResult result = validate_sample(valid_primary(), kNow);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.sample->replication_lag_state(), ReplicationLagState::kNotApplicable);
}

// ---- Whole-sample behaviour ------------------------------------------------------

TEST(ValidationTest, ValidSamplePreservesEveryField) {
    const RawTelemetrySample raw = valid_replica();
    const ValidationResult result = validate_sample(raw, kNow);
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result.issues.empty());
    const TelemetrySample& s = *result.sample;
    EXPECT_EQ(s.node_id().value(), raw.node_id);
    EXPECT_EQ(s.role(), raw.role);
    EXPECT_EQ(s.collected_at(), raw.collected_at);
    EXPECT_EQ(s.cpu_usage_percent(), raw.cpu_usage_percent);
    EXPECT_EQ(s.memory_usage_percent(), raw.memory_usage_percent);
    EXPECT_EQ(s.disk_usage_percent(), raw.disk_usage_percent);
    EXPECT_EQ(s.replication_lag_ms(), raw.replication_lag_ms);
}

TEST(ValidationTest, ReportsAllIssuesInFieldOrder) {
    RawTelemetrySample raw;
    raw.node_id = "";
    raw.role = static_cast<NodeRole>(std::uint8_t{77});
    raw.collected_at = Timestamp{};
    raw.cpu_usage_percent = kNaN;
    raw.memory_usage_percent = 200.0;
    raw.disk_usage_percent = -1.0;
    raw.replication_lag_ms = -kInf;

    const ValidationResult result = validate_sample(raw, kNow);
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.sample.has_value());
    const std::vector<ValidationIssue> expected{
        {Field::kNodeId, ValidationCode::kEmpty},
        {Field::kRole, ValidationCode::kInvalidEnumValue},
        {Field::kCollectedAt, ValidationCode::kTimestampUnset},
        {Field::kCpuUsagePercent, ValidationCode::kNotFinite},
        {Field::kMemoryUsagePercent, ValidationCode::kOutOfRange},
        {Field::kDiskUsagePercent, ValidationCode::kOutOfRange},
        {Field::kReplicationLagMs, ValidationCode::kNotFinite},
    };
    EXPECT_EQ(result.issues, expected);
}

TEST(ValidationTest, ValidationIsDeterministic) {
    RawTelemetrySample raw = valid_replica();
    raw.cpu_usage_percent = kNaN;
    raw.node_id = "bad id";
    const auto first = validate_sample(raw, kNow).issues;
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(validate_sample(raw, kNow).issues, first);
    }
}

// ---- Freshness ---------------------------------------------------------------------

TEST(FreshnessTest, StaleIsAPropertyOfValidData) {
    RawTelemetrySample raw = valid_replica();
    raw.collected_at = kNow - 30s;
    const ValidationResult result = validate_sample(raw, kNow);
    ASSERT_TRUE(result.ok()) << "an old sample is valid; staleness is classified separately";
    EXPECT_EQ(classify_freshness(*result.sample, kNow, 30s), Freshness::kFresh) << "inclusive";
    EXPECT_EQ(classify_freshness(*result.sample, kNow, 29s), Freshness::kStale);
}

TEST(FreshnessTest, SlightlyFutureSampleIsFresh) {
    RawTelemetrySample raw = valid_replica();
    raw.collected_at = kNow + 2s;
    const ValidationResult result = validate_sample(raw, kNow);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(classify_freshness(*result.sample, kNow, 1s), Freshness::kFresh);
}

// ---- String forms are stable (they appear in logs and CLI output) -------------

TEST(ValidationTest, StableStringForms) {
    EXPECT_EQ(to_string(Field::kReplicationLagMs), "replication_lag_ms");
    EXPECT_EQ(to_string(ValidationCode::kReplicationLagNotApplicable),
              "replication_lag_not_applicable");
    EXPECT_EQ(to_string(ReplicationLagState::kNotMeasured), "not_measured");
    EXPECT_EQ(to_string(Freshness::kStale), "stale");
}

}  // namespace
}  // namespace faulttwin::telemetry
