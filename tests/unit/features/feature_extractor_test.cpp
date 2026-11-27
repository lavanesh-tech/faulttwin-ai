#include "faulttwin/features/feature_extractor.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <chrono>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "faulttwin/features/adapters.hpp"
#include "faulttwin/features/feature_schema.hpp"
#include "faulttwin/telemetry/validation.hpp"

namespace {

namespace ft = faulttwin::features;
namespace col = faulttwin::collectors;
using ft::FeatureKind;
using ft::FeatureRef;
using ft::MetricId;
using ft::Observation;
using ft::ObserveResult;
using ft::Quality;
using std::chrono::milliseconds;

const ft::Timestamp kT0{std::chrono::sys_days{std::chrono::year{2026} / 1 / 1}};
ft::Timestamp at_s(std::int64_t s) {
    return kT0 + std::chrono::seconds{s};
}

TEST(FeatureSchemaTest, DefaultSchemaIsVersionedAndPinned) {
    const auto& schema = ft::default_feature_schema();
    EXPECT_EQ(schema.version(), "faulttwin.features.v1");
    EXPECT_EQ(schema.size(), ft::kMetricCount * ft::kFeatureKindCount);
    EXPECT_EQ(schema.window().span, milliseconds{60'000});
    EXPECT_EQ(schema.window().capacity, 256U);
    // Golden fingerprint: changing the feature list, order or window without bumping the
    // version (and this value) fails here on purpose.
    EXPECT_EQ(schema.fingerprint(), 0x5D5E53074F2682A3ULL) << std::hex << schema.fingerprint();
    std::set<std::string> names;
    for (const auto& f : schema.features()) {
        names.insert(ft::feature_name(f));
    }
    EXPECT_EQ(names.size(), schema.size());
    EXPECT_EQ(ft::feature_name(schema.features().front()), "availability.last");
    EXPECT_EQ(ft::feature_name(schema.features().back()), "disk_percent.age_seconds");
}

TEST(FeatureSchemaTest, NamesRoundTripAndBadNamesAreRejected) {
    for (const auto& f : ft::default_feature_schema().features()) {
        const auto parsed = ft::parse_feature_name(ft::feature_name(f));
        ASSERT_TRUE(parsed);
        EXPECT_EQ(*parsed, f);
    }
    EXPECT_FALSE(ft::parse_feature_name("cpu_percent"));
    EXPECT_FALSE(ft::parse_feature_name("cpu_percent.median"));
    EXPECT_FALSE(ft::parse_feature_name("gpu.last"));
}

TEST(FeatureSchemaTest, InvalidSchemasThrow) {
    const ft::WindowConfig window{};
    EXPECT_THROW(ft::FeatureSchema("", window, {{MetricId::kCpuPercent, FeatureKind::kLast}}),
                 std::invalid_argument);
    EXPECT_THROW(ft::FeatureSchema("v", window, {}), std::invalid_argument);
    EXPECT_THROW(ft::FeatureSchema("v", window,
                                   {{MetricId::kCpuPercent, FeatureKind::kLast},
                                    {MetricId::kCpuPercent, FeatureKind::kLast}}),
                 std::invalid_argument);
}

TEST(FeatureSchemaTest, FingerprintDependsOnOrderAndWindow) {
    const ft::WindowConfig window{};
    const ft::FeatureSchema a{
        "v",
        window,
        {{MetricId::kCpuPercent, FeatureKind::kLast}, {MetricId::kCpuPercent, FeatureKind::kMean}}};
    const ft::FeatureSchema b{
        "v",
        window,
        {{MetricId::kCpuPercent, FeatureKind::kMean}, {MetricId::kCpuPercent, FeatureKind::kLast}}};
    ft::WindowConfig other = window;
    other.span = milliseconds{30'000};
    const ft::FeatureSchema c{
        "v",
        other,
        {{MetricId::kCpuPercent, FeatureKind::kLast}, {MetricId::kCpuPercent, FeatureKind::kMean}}};
    EXPECT_NE(a.fingerprint(), b.fingerprint());
    EXPECT_NE(a.fingerprint(), c.fingerprint());
}

TEST(FeatureExtractorTest, NotApplicableIsIgnoredAndStaleCountsAsMissing) {
    ft::FeatureExtractor ex{ft::default_feature_schema()};
    const auto& schema = ex.schema();
    EXPECT_EQ(ex.observe(Observation::missing("p", MetricId::kReplicationLagSeconds, at_s(0),
                                              Quality::kNotApplicable)),
              ObserveResult::kNotApplicable);
    EXPECT_EQ(ex.observe(Observation::ok("r", MetricId::kReplicationLagSeconds, at_s(0), 1.0)),
              ObserveResult::kAccepted);
    Observation stale =
        Observation::missing("r", MetricId::kReplicationLagSeconds, at_s(1), Quality::kStale);
    stale.value = 99.0;  // a stale value is context, never data
    EXPECT_EQ(ex.observe(stale), ObserveResult::kAccepted);

    const auto p = ex.extract("p", at_s(1));
    EXPECT_FALSE(p.get(schema, {MetricId::kReplicationLagSeconds, FeatureKind::kMissingRatio}))
        << "not-applicable is neither data nor missing data";
    const auto r = ex.extract("r", at_s(1));
    EXPECT_EQ(r.get(schema, {MetricId::kReplicationLagSeconds, FeatureKind::kMax}), 1.0);
    EXPECT_EQ(r.get(schema, {MetricId::kReplicationLagSeconds, FeatureKind::kMissingRatio}), 0.5);
    EXPECT_EQ(r.get(schema, {MetricId::kReplicationLagSeconds, FeatureKind::kAgeSeconds}), 1.0);
    EXPECT_EQ(ex.counters().not_applicable, 1U);
}

TEST(FeatureExtractorTest, AgeGrowsWithoutDataAndCountsFromFirstSightWithoutOkData) {
    ft::FeatureExtractor ex{ft::default_feature_schema()};
    const auto& schema = ex.schema();
    (void)ex.observe(Observation::ok("a", MetricId::kAvailability, at_s(10), 1.0));
    (void)ex.observe(Observation::missing("b", MetricId::kAvailability, at_s(12)));
    const FeatureRef age{MetricId::kAvailability, FeatureKind::kAgeSeconds};
    EXPECT_EQ(ex.extract("a", at_s(100)).get(schema, age), 90.0)
        << "age is measured from the newest ok value, even after it left the window";
    EXPECT_EQ(ex.extract("b", at_s(15)).get(schema, age), 3.0);
    EXPECT_FALSE(ex.extract("unknown", at_s(15)).get(schema, age));
}

TEST(FeatureExtractorTest, EventTimeNotArrivalOrderAcrossSeries) {
    // Two interleavings of the same per-series sequences give bit-identical vectors.
    std::vector<Observation> a_obs;
    std::vector<Observation> b_obs;
    for (std::int64_t t = 0; t < 30; ++t) {
        a_obs.push_back(Observation::ok("a", MetricId::kCpuPercent, at_s(t),
                                        40.0 + static_cast<double>(t % 7) * 1.25));
        b_obs.push_back(Observation::ok("b", MetricId::kCpuPercent, at_s(t),
                                        10.0 + static_cast<double>(t % 5) * 3.5));
    }
    ft::FeatureExtractor first{ft::default_feature_schema()};
    ft::FeatureExtractor second{ft::default_feature_schema()};
    for (std::size_t i = 0; i < a_obs.size(); ++i) {
        (void)first.observe(a_obs[i]);
        (void)first.observe(b_obs[i]);
    }
    for (const auto& o : b_obs) {  // all of b first, then all of a
        (void)second.observe(o);
    }
    for (const auto& o : a_obs) {
        (void)second.observe(o);
    }
    for (const std::string node : {"a", "b"}) {
        const auto x = first.extract(node, at_s(29));
        const auto y = second.extract(node, at_s(29));
        ASSERT_EQ(x.values.size(), y.values.size());
        for (std::size_t i = 0; i < x.values.size(); ++i) {
            ASSERT_EQ(x.values[i].has_value(), y.values[i].has_value());
            if (x.values[i]) {
                EXPECT_EQ(std::bit_cast<std::uint64_t>(*x.values[i]),
                          std::bit_cast<std::uint64_t>(*y.values[i]));
            }
        }
    }
}

TEST(FeatureExtractorTest, RefusalsAreCounted) {
    const ft::FeatureSchema schema{
        "v", ft::WindowConfig{}, {{MetricId::kCpuPercent, FeatureKind::kLast}}};
    ft::FeatureExtractor ex{schema, /*max_nodes=*/1};
    EXPECT_EQ(ex.observe(Observation::ok("a", MetricId::kDiskPercent, at_s(0), 1.0)),
              ObserveResult::kNotInSchema);
    EXPECT_EQ(ex.observe(Observation::ok("a", MetricId::kCpuPercent, at_s(1), 1.0)),
              ObserveResult::kAccepted);
    EXPECT_EQ(ex.observe(Observation::ok("a", MetricId::kCpuPercent, at_s(1), 2.0)),
              ObserveResult::kOutOfOrder);
    EXPECT_EQ(ex.observe(Observation::ok("b", MetricId::kCpuPercent, at_s(2), 1.0)),
              ObserveResult::kNodeLimit);
    const auto v = ex.extract("a", at_s(0));  // before the newest entry
    EXPECT_FALSE(v.values.at(0));
    const auto& c = ex.counters();
    EXPECT_EQ(c.not_in_schema, 1U);
    EXPECT_EQ(c.out_of_order, 1U);
    EXPECT_EQ(c.node_limit, 1U);
    EXPECT_EQ(c.clock_behind_data, 1U);
    EXPECT_EQ(ex.nodes(), std::vector<std::string>{"a"});
}

TEST(FeatureExtractorTest, VectorsCarryTheSchemaIdentity) {
    ft::FeatureExtractor ex{ft::default_feature_schema()};
    const auto v = ex.extract("a", at_s(0));
    EXPECT_EQ(v.schema_version, "faulttwin.features.v1");
    EXPECT_EQ(v.schema_fingerprint, ft::default_feature_schema().fingerprint());
    const ft::FeatureSchema other{
        "other", ft::WindowConfig{}, {{MetricId::kCpuPercent, FeatureKind::kLast}}};
    EXPECT_FALSE(v.get(other, {MetricId::kCpuPercent, FeatureKind::kLast}))
        << "a vector is never read through a different schema";
}

// ---- adapters ---------------------------------------------------------------------------

col::PgNodeSnapshot snapshot(std::string name, faulttwin::telemetry::NodeRole role,
                             col::Availability availability) {
    col::PgNodeSnapshot s;
    s.node = std::move(name);
    s.role = role;
    s.availability = availability;
    s.attempted_at = at_s(1);
    s.collected_at = at_s(2);
    s.probe_latency_ms = col::Reading<double>::ok(0.4);
    s.connections.utilization_percent = col::Reading<double>::ok(12.0);
    return s;
}

const Observation* find(const std::vector<Observation>& obs, const std::string& node,
                        MetricId metric) {
    for (const auto& o : obs) {
        if (o.node == node && o.metric == metric) {
            return &o;
        }
    }
    return nullptr;
}

TEST(AdaptersTest, CycleMapsAvailabilityLagAndRates) {
    using faulttwin::telemetry::NodeRole;
    auto primary = snapshot("pg-primary", NodeRole::kPrimary, col::Availability::kUp);
    col::StandbyLink caught_up;
    caught_up.application_name = "pg-replica-1";
    caught_up.replay_lag_bytes = col::Reading<double>::ok(0.0);
    caught_up.replay_lag_seconds = col::Reading<double>::missing("NULL: caught up");
    col::StandbyLink behind;
    behind.application_name = "pg-replica-2";
    behind.replay_lag_bytes = col::Reading<double>::ok(4096.0);
    behind.replay_lag_seconds = col::Reading<double>::ok(2.5);
    primary.standbys = {caught_up, behind};
    const auto r1 = snapshot("pg-replica-1", NodeRole::kReplica, col::Availability::kUp);
    const auto r2 = snapshot("pg-replica-2", NodeRole::kReplica, col::Availability::kUp);
    auto down = snapshot("pg-replica-3", NodeRole::kUnknown, col::Availability::kDown);
    down.probe_latency_ms = col::Reading<double>::missing("connection refused");

    std::vector<col::PgNodeSnapshot> nodes{primary, r1, r2, down};
    col::PgRates rates;
    rates.commits_per_second = col::Reading<double>::ok(250.0);
    rates.mean_statement_latency_ms = col::Reading<double>::missing("no statements");
    std::vector<std::optional<col::PgRates>> node_rates{rates, std::nullopt, std::nullopt,
                                                        std::nullopt};
    const auto obs = ft::observations_from_cycle(nodes, node_rates);

    const auto* avail_down = find(obs, "pg-replica-3", MetricId::kAvailability);
    ASSERT_NE(avail_down, nullptr);
    EXPECT_EQ(avail_down->quality, Quality::kMissing) << "down is missing, never 0";
    EXPECT_EQ(avail_down->at, at_s(1)) << "a down node is timed by the attempt";
    EXPECT_EQ(find(obs, "pg-primary", MetricId::kAvailability)->value, 1.0);
    EXPECT_EQ(find(obs, "pg-primary", MetricId::kAvailability)->at, at_s(2));
    EXPECT_EQ(find(obs, "pg-primary", MetricId::kReplicationLagSeconds)->quality,
              Quality::kNotApplicable);
    const auto* lag1 = find(obs, "pg-replica-1", MetricId::kReplicationLagSeconds);
    EXPECT_EQ(lag1->quality, Quality::kOk);
    EXPECT_EQ(lag1->value, 0.0) << "NULL replay_lag with 0 bytes behind means caught up";
    EXPECT_EQ(find(obs, "pg-replica-2", MetricId::kReplicationLagSeconds)->value, 2.5);
    EXPECT_EQ(find(obs, "pg-replica-2", MetricId::kReplicationLagBytes)->value, 4096.0);
    EXPECT_EQ(find(obs, "pg-replica-3", MetricId::kReplicationLagSeconds)->quality,
              Quality::kMissing);
    EXPECT_EQ(find(obs, "pg-primary", MetricId::kCommitsPerSecond)->value, 250.0);
    EXPECT_EQ(find(obs, "pg-primary", MetricId::kStatementLatencyMs)->quality, Quality::kMissing);
    EXPECT_EQ(find(obs, "pg-replica-1", MetricId::kCommitsPerSecond)->quality, Quality::kMissing);
}

TEST(AdaptersTest, ReplicaLagIsMissingWithoutAPrimaryOrWithTwoPrimaries) {
    using faulttwin::telemetry::NodeRole;
    const auto replica = snapshot("r", NodeRole::kReplica, col::Availability::kUp);
    std::vector<col::PgNodeSnapshot> only_replica{replica};
    auto obs = ft::observations_from_cycle(only_replica, {});
    EXPECT_EQ(find(obs, "r", MetricId::kReplicationLagSeconds)->quality, Quality::kMissing);

    auto p1 = snapshot("p1", NodeRole::kPrimary, col::Availability::kUp);
    col::StandbyLink link;
    link.application_name = "r";
    link.replay_lag_bytes = col::Reading<double>::ok(1.0);
    link.replay_lag_seconds = col::Reading<double>::ok(1.0);
    p1.standbys = {link};
    auto p2 = p1;
    p2.node = "p2";
    std::vector<col::PgNodeSnapshot> split_brain{p1, p2, replica};
    obs = ft::observations_from_cycle(split_brain, {});
    EXPECT_EQ(find(obs, "r", MetricId::kReplicationLagSeconds)->quality, Quality::kMissing);
}

TEST(AdaptersTest, ValidatedSampleMapsToObservations) {
    namespace tel = faulttwin::telemetry;
    tel::RawTelemetrySample raw;
    raw.node_id = "replica-1";
    raw.role = tel::NodeRole::kReplica;
    raw.collected_at = at_s(5);
    raw.cpu_usage_percent = 42.0;
    raw.memory_usage_percent = 50.0;
    raw.disk_usage_percent = 60.0;
    raw.replication_lag_ms = 1500.0;
    const auto result = tel::validate_sample(raw, at_s(5));
    ASSERT_TRUE(result.ok());
    const auto obs = ft::observations_from_sample(*result.sample);
    EXPECT_EQ(find(obs, "replica-1", MetricId::kCpuPercent)->value, 42.0);
    EXPECT_EQ(find(obs, "replica-1", MetricId::kReplicationLagSeconds)->value, 1.5);

    raw.replication_lag_ms.reset();
    const auto missing = tel::validate_sample(raw, at_s(5));
    ASSERT_TRUE(missing.ok());
    const auto obs2 = ft::observations_from_sample(*missing.sample);
    EXPECT_EQ(find(obs2, "replica-1", MetricId::kReplicationLagSeconds)->quality,
              Quality::kMissing);
}

}  // namespace
