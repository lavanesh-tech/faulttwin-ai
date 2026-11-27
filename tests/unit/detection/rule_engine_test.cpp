#include "faulttwin/detection/rule_engine.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "faulttwin/detection/report.hpp"
#include "faulttwin/detection/rule.hpp"

namespace {

namespace det = faulttwin::detection;
namespace ft = faulttwin::features;
using det::Comparison;
using det::DetectionKind;
using det::Severity;
using ft::FeatureKind;
using ft::MetricId;
using std::chrono::milliseconds;
using std::chrono::seconds;

const ft::Timestamp kT0{std::chrono::sys_days{std::chrono::year{2026} / 1 / 1}};
ft::Timestamp at_s(std::int64_t s) {
    return kT0 + seconds{s};
}

const ft::FeatureSchema& schema() {
    static const ft::FeatureSchema s{"test.v1",
                                     ft::WindowConfig{},
                                     {{MetricId::kCpuPercent, FeatureKind::kLast},
                                      {MetricId::kConnectionUtilization, FeatureKind::kLast},
                                      {MetricId::kReplicationLagSeconds, FeatureKind::kLast}}};
    return s;
}

ft::FeatureVector vec(std::int64_t t, std::optional<double> cpu,
                      std::optional<double> connections = std::nullopt,
                      std::optional<double> lag = std::nullopt, std::string node = "n1") {
    ft::FeatureVector v;
    v.schema_version = schema().version();
    v.schema_fingerprint = schema().fingerprint();
    v.node = std::move(node);
    v.as_of = at_s(t);
    v.values = {cpu, connections, lag};
    return v;
}

det::Predicate cpu_above(double trigger, double clear,
                         std::optional<double> critical = std::nullopt) {
    return det::Predicate{
        {MetricId::kCpuPercent, FeatureKind::kLast}, Comparison::kAbove, trigger, clear, critical};
}

det::Rule cpu_rule(seconds sustain = seconds{0}, seconds clear_for = seconds{0},
                   seconds cooldown = seconds{0}, double clear = 80.0) {
    det::Rule r;
    r.id = "cpu";
    r.predicates = {cpu_above(90.0, clear, 98.0)};
    r.sustain = sustain;
    r.clear_for = clear_for;
    r.cooldown = cooldown;
    return r;
}

det::RuleSet set_of(det::Rule rule) {
    return det::RuleSet{"rules.test", {std::move(rule)}};
}

/// Feeds cpu values at t = 0, 1, 2, ... and returns every detection.
std::vector<det::Detection> run(det::RuleEngine& engine, const std::vector<double>& cpu,
                                std::int64_t t0 = 0) {
    std::vector<det::Detection> all;
    for (std::size_t i = 0; i < cpu.size(); ++i) {
        auto r = engine.evaluate(vec(t0 + static_cast<std::int64_t>(i), cpu[i]));
        auto& d = std::get<std::vector<det::Detection>>(r);
        all.insert(all.end(), d.begin(), d.end());
    }
    return all;
}

TEST(RuleValidationTest, CatchesEveryClassOfMistake) {
    det::RuleSet bad{"", {}};
    det::Rule no_predicates;
    no_predicates.id = "empty";
    det::Rule wrong_hysteresis = cpu_rule();
    wrong_hysteresis.id = "hysteresis";
    wrong_hysteresis.predicates = {cpu_above(90.0, 95.0)};
    det::Rule wrong_critical = cpu_rule();
    wrong_critical.id = "critical";
    wrong_critical.predicates = {cpu_above(90.0, 80.0, 85.0)};
    det::Rule unknown_feature = cpu_rule();
    unknown_feature.id = "feature";
    unknown_feature.predicates = {det::Predicate{
        {MetricId::kDiskPercent, FeatureKind::kLast}, Comparison::kAbove, 1.0, 0.5, std::nullopt}};
    det::Rule nan = cpu_rule();
    nan.id = "nan";
    nan.predicates = {cpu_above(std::numeric_limits<double>::quiet_NaN(), 1.0)};
    det::Rule negative = cpu_rule();
    negative.id = "negative";
    negative.sustain = milliseconds{-1};
    det::Rule duplicate = cpu_rule();
    duplicate.id = "negative";
    bad.rules = {no_predicates, wrong_hysteresis, wrong_critical, unknown_feature,
                 nan,           negative,         duplicate};
    const auto issues = det::validate_rule_set(bad, schema());
    std::vector<std::string> ids;
    for (const auto& i : issues) {
        ids.push_back(i.rule_id);
    }
    EXPECT_EQ(ids, (std::vector<std::string>{"", "empty", "hysteresis", "critical", "feature",
                                             "nan", "negative", "negative"}));
    EXPECT_THROW(det::RuleEngine(bad, schema()), std::invalid_argument);
}

TEST(RuleValidationTest, DefaultRuleSetIsValidForTheDefaultSchema) {
    const auto rules = det::default_rule_set();
    EXPECT_EQ(rules.version, "faulttwin.rules.v1");
    EXPECT_EQ(rules.rules.size(), 5U);
    EXPECT_TRUE(det::validate_rule_set(rules, ft::default_feature_schema()).empty());
}

TEST(RuleEngineTest, FiresOnlyAfterTheSustainPeriod) {
    det::RuleEngine engine{set_of(cpu_rule(seconds{3})), schema()};
    const auto d = run(engine, {95, 95, 95, 95, 95});
    ASSERT_EQ(d.size(), 1U);
    EXPECT_EQ(d[0].kind, DetectionKind::kFired);
    EXPECT_EQ(d[0].at, at_s(3)) << "violating since t=0, sustained for 3 s at t=3";
    EXPECT_EQ(d[0].violation_since, at_s(0));
    EXPECT_EQ(d[0].rule_set_version, "rules.test");
    ASSERT_EQ(d[0].evidence.size(), 1U);
    EXPECT_EQ(d[0].evidence[0].feature, "cpu_percent.last");
    EXPECT_EQ(d[0].evidence[0].value, 95.0);
}

TEST(RuleEngineTest, ShortSpikeShorterThanSustainNeverFires) {
    det::RuleEngine engine{set_of(cpu_rule(seconds{3})), schema()};
    EXPECT_TRUE(run(engine, {95, 95, 95, 50, 95, 95, 95, 50}).empty());
}

TEST(RuleEngineTest, HysteresisPreventsFlappingAroundTheTrigger) {
    const std::vector<double> hovering{91, 89, 91, 89, 91, 89, 91, 89, 70};
    det::RuleEngine with{set_of(cpu_rule(seconds{0}, seconds{0}, seconds{0}, 80.0)), schema()};
    const auto d = run(with, hovering);
    ASSERT_EQ(d.size(), 2U) << "one alert, resolved only when CPU drops below 80";
    EXPECT_EQ(d[0].kind, DetectionKind::kFired);
    EXPECT_EQ(d[1].kind, DetectionKind::kResolved);
    EXPECT_EQ(d[1].at, at_s(8));

    det::RuleEngine without{set_of(cpu_rule(seconds{0}, seconds{0}, seconds{0}, 90.0)), schema()};
    const auto naive = run(without, hovering);
    std::size_t fired = 0;
    for (const auto& x : naive) {
        fired += x.kind == DetectionKind::kFired ? 1U : 0U;
    }
    EXPECT_EQ(fired, 4U) << "without hysteresis every crossing is a new alert";
}

TEST(RuleEngineTest, ClearForRequiresARecoveryPeriodAndReViolationContinues) {
    det::RuleEngine engine{set_of(cpu_rule(seconds{0}, seconds{3})), schema()};
    const auto d = run(engine, {95, 50, 50, 95, 50, 50, 50, 50});
    ASSERT_EQ(d.size(), 2U);
    EXPECT_EQ(d[0].kind, DetectionKind::kFired);
    EXPECT_EQ(d[1].kind, DetectionKind::kResolved);
    EXPECT_EQ(d[1].at, at_s(7)) << "recovering from t=4, resolved after 3 s";
    EXPECT_EQ(engine.counters().fired, 1U);
}

TEST(RuleEngineTest, CooldownSuppressesARefireAndCountsIt) {
    det::RuleEngine engine{set_of(cpu_rule(seconds{0}, seconds{0}, seconds{5})), schema()};
    // fire t=0, resolve t=1 (cooldown until t=6), violation t=2..7
    const auto d = run(engine, {95, 50, 95, 95, 95, 95, 95, 95});
    ASSERT_EQ(d.size(), 3U);
    EXPECT_EQ(d[2].kind, DetectionKind::kFired);
    EXPECT_EQ(d[2].at, at_s(6));
    EXPECT_EQ(d[2].violation_since, at_s(2));
    EXPECT_EQ(engine.counters().suppressed_cooldown, 4U);  // t = 2, 3, 4, 5
}

TEST(RuleEngineTest, EscalatesOnceAndFiresCriticalWhenAlreadyBeyond) {
    det::RuleEngine engine{set_of(cpu_rule()), schema()};
    const auto d = run(engine, {95, 99, 99, 95});
    ASSERT_EQ(d.size(), 2U);
    EXPECT_EQ(d[0].severity, Severity::kWarning);
    EXPECT_EQ(d[1].kind, DetectionKind::kEscalated);
    EXPECT_EQ(d[1].severity, Severity::kCritical);

    det::RuleEngine direct{set_of(cpu_rule()), schema()};
    const auto c = run(direct, {99});
    ASSERT_EQ(c.size(), 1U);
    EXPECT_EQ(c[0].kind, DetectionKind::kFired);
    EXPECT_EQ(c[0].severity, Severity::kCritical);
}

TEST(RuleEngineTest, MissingDataResetsPendingAndHoldsFiring) {
    det::RuleEngine pending{set_of(cpu_rule(seconds{2})), schema()};
    std::vector<det::Detection> d;
    for (const auto& v : {vec(0, 95.0), vec(1, std::nullopt), vec(2, 95.0), vec(3, 95.0)}) {
        auto r = std::get<std::vector<det::Detection>>(pending.evaluate(v));
        d.insert(d.end(), r.begin(), r.end());
    }
    EXPECT_TRUE(d.empty()) << "the sustain window must be covered by data";
    EXPECT_EQ(pending.counters().unknown, 1U);

    det::RuleEngine firing{set_of(cpu_rule()), schema()};
    (void)firing.evaluate(vec(0, 95.0));
    for (std::int64_t t = 1; t < 10; ++t) {
        EXPECT_TRUE(
            std::get<std::vector<det::Detection>>(firing.evaluate(vec(t, std::nullopt))).empty());
    }
    EXPECT_EQ(firing.firing().size(), 1U) << "missing data does not clear an alert";
}

TEST(RuleEngineTest, MultiSignalAllAndAnyAreThreeValued) {
    det::Rule all;
    all.id = "all";
    all.combiner = det::Combiner::kAll;
    all.predicates = {cpu_above(90.0, 80.0),
                      det::Predicate{{MetricId::kConnectionUtilization, FeatureKind::kLast},
                                     Comparison::kAbove,
                                     60.0,
                                     50.0,
                                     std::nullopt}};
    det::Rule any = all;
    any.id = "any";
    any.combiner = det::Combiner::kAny;
    det::RuleEngine engine{det::RuleSet{"v", {all, any}}, schema()};

    auto fired = [&](const ft::FeatureVector& v) {
        std::vector<std::string> ids;
        const auto result = engine.evaluate(v);
        for (const auto& d : std::get<std::vector<det::Detection>>(result)) {
            if (d.kind == DetectionKind::kFired) {
                ids.push_back(d.rule_id);
            }
        }
        return ids;
    };
    EXPECT_EQ(fired(vec(0, 95.0, 10.0)), std::vector<std::string>{"any"});
    EXPECT_TRUE(fired(vec(1, 95.0, std::nullopt)).empty()) << "all: true AND unknown";
    EXPECT_EQ(fired(vec(2, 95.0, 70.0)), std::vector<std::string>{"all"});
}

TEST(RuleEngineTest, BelowComparisonAndNodesAreIndependent) {
    det::Rule low;
    low.id = "low_connections";
    low.predicates = {det::Predicate{{MetricId::kConnectionUtilization, FeatureKind::kLast},
                                     Comparison::kBelow,
                                     5.0,
                                     10.0,
                                     1.0}};
    det::RuleEngine engine{set_of(low), schema()};
    auto a = std::get<std::vector<det::Detection>>(
        engine.evaluate(vec(0, std::nullopt, 3.0, std::nullopt, "a")));
    auto b = std::get<std::vector<det::Detection>>(
        engine.evaluate(vec(0, std::nullopt, 50.0, std::nullopt, "b")));
    ASSERT_EQ(a.size(), 1U);
    EXPECT_EQ(a[0].node, "a");
    EXPECT_TRUE(b.empty());
    auto still = std::get<std::vector<det::Detection>>(
        engine.evaluate(vec(1, std::nullopt, 7.0, std::nullopt, "a")));
    EXPECT_TRUE(still.empty()) << "7 is above the trigger but below clear (10): still firing";
}

TEST(RuleEngineTest, RejectsForeignSchemaTimeTravelAndTooManyNodes) {
    det::RuleEngine engine{set_of(cpu_rule()), schema(), /*max_nodes=*/1};
    auto foreign = vec(0, 95.0);
    foreign.schema_version = "other";
    EXPECT_EQ(std::get<det::EvaluateError>(engine.evaluate(foreign)),
              det::EvaluateError::kSchemaMismatch);
    (void)engine.evaluate(vec(5, 50.0));
    EXPECT_EQ(std::get<det::EvaluateError>(engine.evaluate(vec(4, 50.0))),
              det::EvaluateError::kTimeWentBackwards);
    EXPECT_EQ(std::get<det::EvaluateError>(
                  engine.evaluate(vec(6, 50.0, std::nullopt, std::nullopt, "other"))),
              det::EvaluateError::kNodeLimit);
    EXPECT_EQ(engine.counters().rejected_vectors, 3U);
}

TEST(RuleEngineTest, DeterministicForTheSameInput) {
    std::vector<double> cpu;
    std::uint64_t x = 7;
    for (int i = 0; i < 500; ++i) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        cpu.push_back(70.0 + static_cast<double>(x >> 58U));
    }
    det::RuleEngine a{set_of(cpu_rule(seconds{2}, seconds{2}, seconds{5})), schema()};
    det::RuleEngine b{set_of(cpu_rule(seconds{2}, seconds{2}, seconds{5})), schema()};
    std::string ja;
    std::string jb;
    for (const auto& d : run(a, cpu)) {
        ja += det::detection_json(d) + "\n";
    }
    for (const auto& d : run(b, cpu)) {
        jb += det::detection_json(d) + "\n";
    }
    EXPECT_FALSE(ja.empty());
    EXPECT_EQ(ja, jb);
}

TEST(DetectionReportTest, JsonIsStableAndEscaped) {
    det::Detection d;
    d.kind = DetectionKind::kFired;
    d.rule_set_version = "faulttwin.rules.v1";
    d.rule_id = "cpu";
    d.node = "n\"1";
    d.severity = Severity::kCritical;
    d.at = at_s(65);
    d.violation_since = at_s(60);
    d.evidence = {{"cpu_percent.last", 97.5}, {"connection_utilization.last", std::nullopt}};
    EXPECT_EQ(det::detection_json(d),
              R"({"type":"detection","kind":"fired","rule_set":"faulttwin.rules.v1",)"
              R"("rule":"cpu","node":"n\"1","severity":"critical",)"
              R"("at":"2026-01-01T00:01:05.000Z","violation_since":"2026-01-01T00:01:00.000Z",)"
              R"("evidence":{"cpu_percent.last":97.5,"connection_utilization.last":null}})");
    EXPECT_EQ(det::json_number(std::numeric_limits<double>::infinity()), "null");
    EXPECT_EQ(det::iso8601(kT0 + milliseconds{3'723'004}), "2026-01-01T01:02:03.004Z");
}

}  // namespace
