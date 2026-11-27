#include <gtest/gtest.h>

#include <bit>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "faulttwin/detection/evaluation.hpp"
#include "faulttwin/detection/scenario.hpp"

namespace {

namespace det = faulttwin::detection;
namespace ft = faulttwin::features;
using std::chrono::seconds;

std::uint64_t hash_observations(const det::Scenario& s) {
    std::uint64_t h = 14695981039346656037ULL;
    const auto mix = [&](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (8U * static_cast<unsigned>(i))) & 0xFFU;
            h *= 1099511628211ULL;
        }
    };
    for (const auto& o : s.observations) {
        for (const char c : o.node) {
            mix(static_cast<unsigned char>(c));
        }
        mix(static_cast<std::uint64_t>(o.metric));
        // Fixed unit: system_clock ticks are nanoseconds in libstdc++ but microseconds in
        // libc++ (macOS), so the raw count() would differ for identical timestamps.
        mix(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(o.at.time_since_epoch()).count()));
        mix(static_cast<std::uint64_t>(o.quality));
        mix(std::bit_cast<std::uint64_t>(o.value));
    }
    return h;
}

TEST(ScenarioTest, SameSeedSameScenarioDifferentSeedDifferentScenario) {
    det::ScenarioConfig config;
    config.seed = 11;
    const auto a = det::generate_scenario(config);
    const auto b = det::generate_scenario(config);
    config.seed = 12;
    const auto c = det::generate_scenario(config);
    EXPECT_EQ(hash_observations(a), hash_observations(b));
    EXPECT_NE(hash_observations(a), hash_observations(c));
    EXPECT_EQ(a.episodes.size(), 3U);
    EXPECT_EQ(a.distractors.size(), 5U);
    EXPECT_EQ(a.nodes, (std::vector<std::string>{"primary", "replica-1", "replica-2"}));
}

TEST(ScenarioTest, GoldenHashPinsCrossPlatformDeterminism) {
    // Uses only +, -, *, / and an in-tree PRNG, compiled with -ffp-contract=off, so the
    // observations must be bit-identical on every compiler and CPU. If this fails on one
    // platform only, determinism is broken there; if it fails everywhere, the generator
    // changed and the value must be updated together with the evaluation results.
    det::ScenarioConfig config;
    config.seed = 1;
    const auto s = det::generate_scenario(config);
    EXPECT_EQ(hash_observations(s), 0x91CDA28CB941ACF1ULL) << std::hex << hash_observations(s);
}

TEST(ScenarioTest, ObservationsAreOrderedAndGroundTruthIsInsideTheRun) {
    det::ScenarioConfig config;
    config.seed = 5;
    const auto s = det::generate_scenario(config);
    std::map<std::tuple<std::string, ft::MetricId>, ft::Timestamp> last;
    for (std::size_t i = 1; i < s.observations.size(); ++i) {
        EXPECT_LE(s.observations[i - 1].at, s.observations[i].at);
    }
    for (const auto& o : s.observations) {
        const auto key = std::make_tuple(o.node, o.metric);
        if (const auto it = last.find(key); it != last.end()) {
            EXPECT_LT(it->second, o.at) << "one observation per series per tick";
        }
        last[key] = o.at;
    }
    const auto begin = config.start + seconds{60};
    const auto end = config.start + config.duration - seconds{60};
    for (const auto& e : s.episodes) {
        EXPECT_GE(e.onset, begin);
        EXPECT_LE(e.end, end);
        EXPECT_LT(e.onset, e.end);
    }
    for (std::size_t i = 1; i < s.episodes.size(); ++i) {
        EXPECT_LE(s.episodes[i - 1].end, s.episodes[i].onset) << "episodes never overlap";
    }
}

// ---- scoring ------------------------------------------------------------------------------

det::Scenario hand_made() {
    det::Scenario s;
    s.config.duration = seconds{3600};
    s.nodes = {"a", "b"};
    const auto t = [&](std::int64_t sec) { return s.config.start + seconds{sec}; };
    s.episodes.push_back({"a", det::FaultKind::kCpuSaturation, t(100), t(200), 1.0});
    s.episodes.push_back({"a", det::FaultKind::kNodeDown, t(1000), t(1100), 1.0});
    s.episodes.push_back({"b", det::FaultKind::kOverload, t(500), t(600), 1.0});
    return s;
}

det::Detection fired(const det::Scenario& s, const std::string& node, std::int64_t sec,
                     const std::string& rule = "r") {
    det::Detection d;
    d.kind = det::DetectionKind::kFired;
    d.rule_id = rule;
    d.node = node;
    d.at = s.config.start + seconds{sec};
    return d;
}

TEST(ScoringTest, MatchesByNodeAndWindowWithGrace) {
    const auto s = hand_made();
    std::vector<det::Detection> d{
        fired(s, "a", 99),    // before onset: false alert
        fired(s, "a", 110),   // TP cpu, latency 10
        fired(s, "a", 150),   // second alert in the same episode: matched, not a new TP
        fired(s, "a", 230),   // within grace (30 s): matched
        fired(s, "a", 231),   // after grace: false
        fired(s, "b", 1050),  // node b has no episode then: false
    };
    det::Detection resolved = fired(s, "a", 120);
    resolved.kind = det::DetectionKind::kResolved;
    d.push_back(resolved);  // only kFired counts as an alert

    const auto c = det::score(s, d, det::MatchConfig{seconds{30}});
    EXPECT_EQ(c.alerts, 6U);
    EXPECT_EQ(c.matched_alerts, 3U);
    EXPECT_EQ(c.false_alerts, 3U);
    EXPECT_EQ(c.episodes, 3U);
    EXPECT_EQ(c.detected, 1U);
    const auto& cpu = c.by_kind.at(static_cast<std::size_t>(det::FaultKind::kCpuSaturation));
    EXPECT_EQ(cpu.detected, 1U);
    EXPECT_EQ(cpu.latencies_seconds, std::vector<double>{10.0});
    // 2 nodes x 1 h minus (130 + 130) s on a and 130 s on b
    EXPECT_NEAR(c.negative_hours, (7200.0 - 390.0) / 3600.0, 1e-12);

    const auto m = det::metrics(c);
    EXPECT_DOUBLE_EQ(*m.precision, 0.5);
    EXPECT_DOUBLE_EQ(*m.recall, 1.0 / 3.0);
    EXPECT_DOUBLE_EQ(*m.f1, 0.4);
    EXPECT_DOUBLE_EQ(*m.missed_rate, 2.0 / 3.0);
    EXPECT_NEAR(*m.false_alerts_per_hour, 3.0 / ((7200.0 - 390.0) / 3600.0), 1e-12);
}

TEST(ScoringTest, TargetFilterIgnoresOtherEpisodes) {
    const auto s = hand_made();
    const std::vector<det::Detection> d{fired(s, "a", 1010), fired(s, "a", 2000)};
    const auto c = det::score(s, d, det::MatchConfig{},
                              [](det::FaultKind k) { return k == det::FaultKind::kCpuSaturation; });
    EXPECT_EQ(c.alerts, 1U) << "the alert inside the node-down episode is not counted";
    EXPECT_EQ(c.false_alerts, 1U);
    EXPECT_EQ(c.episodes, 1U);
}

TEST(ScoringTest, UndefinedRatiosAreNotApplicable) {
    const det::EvaluationCounts empty;
    const auto m = det::metrics(empty);
    EXPECT_FALSE(m.precision);
    EXPECT_FALSE(m.recall);
    EXPECT_FALSE(m.f1);
    EXPECT_FALSE(m.false_alerts_per_hour);
    EXPECT_FALSE(m.latency_median_seconds);
}

TEST(ScoringTest, NearestRankPercentile) {
    EXPECT_FALSE(det::nearest_rank_percentile({}, 50.0));
    EXPECT_EQ(det::nearest_rank_percentile({5.0, 1.0, 3.0, 2.0, 4.0}, 50.0), 3.0);
    EXPECT_EQ(det::nearest_rank_percentile({5.0, 1.0, 3.0, 2.0}, 50.0), 2.0);
    EXPECT_EQ(det::nearest_rank_percentile({5.0, 1.0, 3.0, 2.0}, 95.0), 5.0);
    EXPECT_EQ(det::nearest_rank_percentile({7.0}, 0.0), 7.0);
}

TEST(EndToEndTest, ExtractionHasNoLookAhead) {
    det::ScenarioConfig config;
    config.seed = 3;
    config.duration = seconds{200};
    const auto s = det::generate_scenario(config);
    const auto full = det::extract_scenario_features(s, ft::default_feature_schema());
    // Truncating the observations after t = 120 s must not change any vector up to 120 s.
    det::Scenario cut = s;
    std::erase_if(cut.observations,
                  [&](const ft::Observation& o) { return o.at > s.config.start + seconds{120}; });
    const auto partial = det::extract_scenario_features(cut, ft::default_feature_schema());
    std::size_t compared = 0;
    for (std::size_t i = 0; i < full.vectors.size(); ++i) {
        if (full.vectors[i].as_of > s.config.start + seconds{120}) {
            break;
        }
        ASSERT_EQ(full.vectors[i].values, partial.vectors[i].values) << i;
        ++compared;
    }
    EXPECT_EQ(compared, 121U * 3U);
}

TEST(EndToEndTest, TuningSeesOnlyTheScenariosItIsGiven) {
    // Tuning on an empty validation set has no information: every grid row has no episodes,
    // so nothing is chosen and the defaults stay. This is the code path that guarantees
    // test scenarios cannot influence thresholds: tune_default_rules only receives the
    // validation scenarios (the CLI also refuses overlapping seed ranges).
    const auto result = det::tune_default_rules({}, {}, ft::default_feature_schema(),
                                                det::BaselineKind::kTunedRules, det::MatchConfig{});
    const det::DefaultRuleParams defaults;
    EXPECT_EQ(result.params.lag_trigger_seconds, defaults.lag_trigger_seconds);
    for (const auto& row : result.grid) {
        EXPECT_EQ(row.episodes, 0U);
    }
}

TEST(EndToEndTest, TunedRulesBeatTheNaiveBaselineOnFalseAlertsOnASmallValidationSet) {
    std::vector<det::Scenario> scenarios;
    std::vector<det::ScenarioFeatures> features;
    for (std::uint64_t seed = 1; seed <= 4; ++seed) {
        det::ScenarioConfig config;
        config.seed = seed;
        scenarios.push_back(det::generate_scenario(config));
        features.push_back(
            det::extract_scenario_features(scenarios.back(), ft::default_feature_schema()));
    }
    const auto& schema = ft::default_feature_schema();
    const auto tuned =
        det::tune_default_rules(scenarios, features, schema, det::BaselineKind::kTunedRules, {});
    const auto naive = det::tune_default_rules(scenarios, features, schema,
                                               det::BaselineKind::kNaiveThreshold, {});
    std::size_t chosen = 0;
    for (const auto& row : tuned.grid) {
        chosen += row.chosen ? 1U : 0U;
    }
    EXPECT_EQ(chosen, 5U) << "one choice per rule";
    EXPECT_EQ(naive.params.clear_ratio, 1.0);
    EXPECT_EQ(naive.params.cooldown.count(), 0);

    det::EvaluationCounts t;
    det::EvaluationCounts n;
    for (std::size_t i = 0; i < scenarios.size(); ++i) {
        t.merge(det::score(scenarios[i],
                           det::run_rules(features[i], det::default_rule_set(tuned.params), schema),
                           {}));
        n.merge(det::score(scenarios[i],
                           det::run_rules(features[i], det::default_rule_set(naive.params), schema),
                           {}));
    }
    EXPECT_LE(t.false_alerts, n.false_alerts);
}

}  // namespace
