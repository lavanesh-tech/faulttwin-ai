#include "faulttwin/features/rolling_window.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "faulttwin/features/feature_extractor.hpp"

namespace {

using faulttwin::features::PushResult;
using faulttwin::features::RollingWindow;
using faulttwin::features::Timestamp;
using faulttwin::features::WelfordAccumulator;
using faulttwin::features::WindowConfig;
using std::chrono::milliseconds;
using std::chrono::seconds;

const Timestamp kT0{std::chrono::sys_days{std::chrono::year{2026} / 1 / 1}};

Timestamp at_ms(std::int64_t ms) {
    return kT0 + milliseconds{ms};
}

WindowConfig config(std::int64_t span_ms, std::size_t capacity = 256, std::size_t reseed = 1024) {
    WindowConfig c;
    c.span = milliseconds{span_ms};
    c.capacity = capacity;
    c.reseed_interval = reseed;
    return c;
}

/// Exact-as-possible reference: two-pass sample variance.
double two_pass_variance(const std::vector<double>& v) {
    double mean = 0.0;
    for (const double x : v) {
        mean += x;
    }
    mean /= static_cast<double>(v.size());
    double ss = 0.0;
    for (const double x : v) {
        ss += (x - mean) * (x - mean);
    }
    return ss / static_cast<double>(v.size() - 1);
}

TEST(WelfordTest, AvoidsCatastrophicCancellationWithALargeOffset) {
    // Sample variance of {4, 7, 13, 16} is exactly 30. With an offset of 1e9 the textbook
    // E[x^2] - E[x]^2 formula loses every significant digit in double precision.
    WelfordAccumulator w;
    double sum = 0.0;
    double sum_sq = 0.0;
    for (const double x : {4.0, 7.0, 13.0, 16.0}) {
        w.add(1e9 + x);
        sum += 1e9 + x;
        sum_sq += (1e9 + x) * (1e9 + x);
    }
    ASSERT_TRUE(w.sample_variance().has_value());
    EXPECT_DOUBLE_EQ(*w.sample_variance(), 30.0);
    EXPECT_DOUBLE_EQ(w.mean(), 1e9 + 10.0);

    const double naive = (sum_sq - sum * sum / 4.0) / 3.0;
    EXPECT_GT(std::abs(naive - 30.0), 1.0) << "the naive formula is expected to fail here";
}

TEST(WelfordTest, RemoveIsTheInverseOfAdd) {
    WelfordAccumulator w;
    for (const double x : {3.0, 5.0, 11.0, 2.0}) {
        w.add(x);
    }
    w.remove(3.0);
    ASSERT_EQ(w.count(), 3U);
    EXPECT_NEAR(w.mean(), 6.0, 1e-12);
    EXPECT_NEAR(*w.sample_variance(), two_pass_variance({5.0, 11.0, 2.0}), 1e-12);
    w.remove(5.0);
    w.remove(11.0);
    EXPECT_FALSE(w.sample_variance().has_value()) << "one value has no sample variance";
    w.remove(2.0);
    EXPECT_EQ(w.count(), 0U);
    EXPECT_EQ(w.mean(), 0.0);
}

TEST(RollingWindowTest, WindowIsHalfOpenOnTheOldSide) {
    RollingWindow w{config(10'000)};
    ASSERT_EQ(w.push(at_ms(0), 1.0, true), PushResult::kAccepted);
    ASSERT_EQ(w.push(at_ms(5'000), 2.0, true), PushResult::kAccepted);
    auto s = w.stats(at_ms(9'999));
    ASSERT_TRUE(s);
    EXPECT_EQ(s->entries, 2U);
    s = w.stats(at_ms(10'000));  // (0, 10000]: the entry at 0 is out
    ASSERT_TRUE(s);
    EXPECT_EQ(s->entries, 1U);
    EXPECT_EQ(s->last, 2.0);
    EXPECT_EQ(w.counters().span_evictions, 1U);
}

TEST(RollingWindowTest, OutOfOrderDuplicateAndNonFiniteAreRefusedAndCounted) {
    RollingWindow w{config(10'000)};
    EXPECT_EQ(w.push(at_ms(1'000), 1.0, true), PushResult::kAccepted);
    EXPECT_EQ(w.push(at_ms(1'000), 2.0, true), PushResult::kOutOfOrder);
    EXPECT_EQ(w.push(at_ms(500), 2.0, true), PushResult::kOutOfOrder);
    EXPECT_EQ(w.push(at_ms(2'000), std::numeric_limits<double>::quiet_NaN(), true),
              PushResult::kNonFinite);
    EXPECT_EQ(w.push(at_ms(2'000), std::numeric_limits<double>::infinity(), true),
              PushResult::kNonFinite);
    // A not-ok marker carries no value, so its payload is irrelevant.
    EXPECT_EQ(w.push(at_ms(2'000), std::numeric_limits<double>::quiet_NaN(), false),
              PushResult::kAccepted);
    EXPECT_EQ(w.counters().out_of_order, 2U);
    EXPECT_EQ(w.counters().non_finite, 2U);
    EXPECT_EQ(w.counters().accepted, 2U);
}

TEST(RollingWindowTest, MissingEntriesCountButNeverBecomeNumbers) {
    RollingWindow w{config(60'000)};
    (void)w.push(at_ms(0), 10.0, true);
    (void)w.push(at_ms(1'000), 0.0, false);
    (void)w.push(at_ms(2'000), 0.0, false);
    (void)w.push(at_ms(3'000), 20.0, true);
    const auto s = w.stats(at_ms(3'000));
    ASSERT_TRUE(s);
    EXPECT_EQ(s->entries, 4U);
    EXPECT_EQ(s->ok_entries, 2U);
    EXPECT_EQ(s->missing_ratio, 0.5);
    EXPECT_EQ(s->mean, 15.0);
    EXPECT_EQ(s->min, 10.0);
    EXPECT_EQ(s->rate_per_second, 10.0 / 3.0);
}

TEST(RollingWindowTest, OnlyMissingDataGivesNoStatistics) {
    RollingWindow w{config(60'000)};
    (void)w.push(at_ms(0), 0.0, false);
    const auto s = w.stats(at_ms(0));
    ASSERT_TRUE(s);
    EXPECT_EQ(s->missing_ratio, 1.0);
    EXPECT_FALSE(s->last);
    EXPECT_FALSE(s->mean);
    EXPECT_FALSE(s->stddev);
    EXPECT_FALSE(s->slope_per_second);

    RollingWindow empty{config(60'000)};
    const auto e = empty.stats(at_ms(0));
    ASSERT_TRUE(e);
    EXPECT_EQ(e->entries, 0U);
    EXPECT_FALSE(e->missing_ratio) << "an empty window has no missing ratio, not 0";
}

TEST(RollingWindowTest, StatsBeforeTheNewestEntryAreRefused) {
    RollingWindow w{config(60'000)};
    (void)w.push(at_ms(5'000), 1.0, true);
    EXPECT_FALSE(w.stats(at_ms(4'999)).has_value());
    EXPECT_TRUE(w.stats(at_ms(5'000)).has_value());
}

TEST(RollingWindowTest, CapacityBoundsMemoryAndCountsEvictions) {
    RollingWindow w{config(1'000'000, 4)};
    for (int i = 0; i < 10; ++i) {
        (void)w.push(at_ms(i * 1'000), static_cast<double>(i), true);
    }
    EXPECT_EQ(w.size(), 4U);
    EXPECT_EQ(w.counters().capacity_evictions, 6U);
    const auto s = w.stats(at_ms(9'000));
    ASSERT_TRUE(s);
    EXPECT_EQ(s->min, 6.0);
    EXPECT_EQ(s->mean, 7.5);
}

TEST(RollingWindowTest, SlopeOfALinearSeriesIsExactEvenWithIrregularSpacing) {
    RollingWindow w{config(60'000)};
    for (const std::int64_t ms : {0, 700, 1'000, 4'200, 9'000, 9'100}) {
        const double t = static_cast<double>(ms) / 1000.0;
        (void)w.push(at_ms(ms), 1e6 + 2.5 * t, true);
    }
    const auto s = w.stats(at_ms(9'100));
    ASSERT_TRUE(s && s->slope_per_second && s->rate_per_second);
    EXPECT_NEAR(*s->slope_per_second, 2.5, 1e-9);
    EXPECT_NEAR(*s->rate_per_second, 2.5, 1e-9);
}

TEST(RollingWindowTest, LongSlidingRunStaysCloseToAnExactRecomputation) {
    // 200,000 pushes through a 64-entry window with large, varied values: incremental
    // removal accumulates rounding error, which the periodic re-seed bounds.
    RollingWindow w{config(64'000, 64, 1024)};
    std::uint64_t x = 12345;
    for (std::int64_t i = 0; i < 200'000; ++i) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        const double v = 1e8 + static_cast<double>(x >> 40U) / 16.0;
        (void)w.push(at_ms(i * 1'000), v, true);
    }
    std::vector<double> values;
    for (const auto& e : w.entries()) {
        values.push_back(e.value);
    }
    const auto s = w.stats(at_ms(199'999'000));
    ASSERT_TRUE(s && s->stddev);
    const double exact = std::sqrt(two_pass_variance(values));
    EXPECT_NEAR(*s->stddev, exact, exact * 1e-9);
    EXPECT_GT(w.counters().reseeds, 100U);
}

TEST(RollingWindowTest, InvalidConfigurationThrows) {
    EXPECT_THROW(RollingWindow{config(0)}, std::invalid_argument);
    EXPECT_THROW(RollingWindow{config(1'000, 0)}, std::invalid_argument);
    EXPECT_THROW(RollingWindow{config(1'000, 4, 0)}, std::invalid_argument);
}

struct GoldenEntry {
    std::int64_t ms;
    double value;
    bool ok;
};
struct GoldenExpectation {
    std::int64_t as_of_ms;
    std::size_t entries;
    std::size_t ok_entries;
    std::optional<double> last, mean, stddev, min, max, rate, slope, missing_ratio, age;
};

#include "golden_features.inc"

void expect_close(std::optional<double> actual, std::optional<double> expected, const char* what,
                  std::int64_t as_of) {
    ASSERT_EQ(actual.has_value(), expected.has_value()) << what << " at " << as_of;
    if (expected) {
        // metric spec §6.4: equality within 1e-9 relative tolerance (absolute near zero).
        const double tolerance = std::max(std::abs(*expected) * 1e-9, 1e-9);
        EXPECT_NEAR(*actual, *expected, tolerance) << what << " at " << as_of;
    }
}

TEST(RollingWindowTest, MatchesTheExactRationalReference) {
    RollingWindow w{config(kGoldenSpanMs)};
    std::size_t next = 0;
    for (const auto& expected : kGoldenExpected) {
        while (next < kGoldenSeries.size() && kGoldenSeries[next].ms <= expected.as_of_ms) {
            ASSERT_EQ(w.push(at_ms(kGoldenSeries[next].ms), kGoldenSeries[next].value,
                             kGoldenSeries[next].ok),
                      PushResult::kAccepted);
            ++next;
        }
        const auto s = w.stats(at_ms(expected.as_of_ms));
        ASSERT_TRUE(s);
        EXPECT_EQ(s->entries, expected.entries) << expected.as_of_ms;
        EXPECT_EQ(s->ok_entries, expected.ok_entries) << expected.as_of_ms;
        expect_close(s->last, expected.last, "last", expected.as_of_ms);
        expect_close(s->mean, expected.mean, "mean", expected.as_of_ms);
        expect_close(s->stddev, expected.stddev, "stddev", expected.as_of_ms);
        expect_close(s->min, expected.min, "min", expected.as_of_ms);
        expect_close(s->max, expected.max, "max", expected.as_of_ms);
        expect_close(s->rate_per_second, expected.rate, "rate", expected.as_of_ms);
        expect_close(s->slope_per_second, expected.slope, "slope", expected.as_of_ms);
        expect_close(s->missing_ratio, expected.missing_ratio, "missing_ratio", expected.as_of_ms);
    }
}

TEST(FeatureExtractorTest, MatchesTheExactRationalReferenceIncludingAge) {
    using faulttwin::features::FeatureExtractor;
    using faulttwin::features::FeatureKind;
    using faulttwin::features::FeatureRef;
    using faulttwin::features::FeatureSchema;
    using faulttwin::features::MetricId;
    using faulttwin::features::Observation;
    using faulttwin::features::Quality;

    const MetricId m = MetricId::kCpuPercent;
    FeatureSchema schema{"golden.v1",
                         config(kGoldenSpanMs),
                         {{m, FeatureKind::kLast},
                          {m, FeatureKind::kMean},
                          {m, FeatureKind::kStddev},
                          {m, FeatureKind::kMin},
                          {m, FeatureKind::kMax},
                          {m, FeatureKind::kRatePerSecond},
                          {m, FeatureKind::kSlopePerSecond},
                          {m, FeatureKind::kMissingRatio},
                          {m, FeatureKind::kAgeSeconds}}};
    FeatureExtractor extractor{schema};
    std::size_t next = 0;
    for (const auto& expected : kGoldenExpected) {
        while (next < kGoldenSeries.size() && kGoldenSeries[next].ms <= expected.as_of_ms) {
            const auto& e = kGoldenSeries[next];
            const Observation o = e.ok ? Observation::ok("n", m, at_ms(e.ms), e.value)
                                       : Observation::missing("n", m, at_ms(e.ms), Quality::kStale);
            (void)extractor.observe(o);
            ++next;
        }
        const auto v = extractor.extract("n", at_ms(expected.as_of_ms));
        const auto get = [&](FeatureKind kind) { return v.get(schema, FeatureRef{m, kind}); };
        expect_close(get(FeatureKind::kLast), expected.last, "last", expected.as_of_ms);
        expect_close(get(FeatureKind::kMean), expected.mean, "mean", expected.as_of_ms);
        expect_close(get(FeatureKind::kStddev), expected.stddev, "stddev", expected.as_of_ms);
        expect_close(get(FeatureKind::kMin), expected.min, "min", expected.as_of_ms);
        expect_close(get(FeatureKind::kMax), expected.max, "max", expected.as_of_ms);
        expect_close(get(FeatureKind::kRatePerSecond), expected.rate, "rate", expected.as_of_ms);
        expect_close(get(FeatureKind::kSlopePerSecond), expected.slope, "slope", expected.as_of_ms);
        expect_close(get(FeatureKind::kMissingRatio), expected.missing_ratio, "missing_ratio",
                     expected.as_of_ms);
        expect_close(get(FeatureKind::kAgeSeconds), expected.age, "age", expected.as_of_ms);
    }
}

}  // namespace
