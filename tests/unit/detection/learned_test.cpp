#include "faulttwin/detection/learned.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace det = faulttwin::detection;
namespace ft = faulttwin::features;
using std::chrono::milliseconds;
using std::chrono::seconds;

const ft::Timestamp kT0{std::chrono::sys_days{std::chrono::year{2026} / 1 / 1}};

const std::function<std::vector<det::Evidence>()> kNoEvidence = [] {
    return std::vector<det::Evidence>{};
};

TEST(LearnedTest, ProjectionPicksTheLearnedFeatures) {
    const auto& schema = ft::default_feature_schema();
    const det::Projection projection{schema};
    ASSERT_EQ(projection.dims(), det::learned_features().size());
    ft::FeatureVector v;
    v.values.assign(schema.size(), std::nullopt);
    const auto cpu = schema.index_of({ft::MetricId::kCpuPercent, ft::FeatureKind::kLast});
    ASSERT_TRUE(cpu);
    v.values[*cpu] = 97.0;
    const auto raw = projection.raw(v);
    EXPECT_EQ(raw[2], std::optional<double>{97.0});  // cpu_percent.last is the third feature
    EXPECT_FALSE(raw[0]);

    const ft::FeatureSchema small{
        "small", ft::WindowConfig{}, {{ft::MetricId::kCpuPercent, ft::FeatureKind::kLast}}};
    EXPECT_THROW(det::Projection{small}, std::invalid_argument);
}

TEST(LearnedTest, StandardizerUsesPresentValuesAndClips) {
    const std::vector<std::vector<std::optional<double>>> rows{
        {1.0, std::nullopt}, {2.0, 5.0}, {3.0, 5.0}, {std::nullopt, 5.0}};
    const auto s = det::Standardizer::fit(rows, 2);
    EXPECT_DOUBLE_EQ(s.means()[0], 2.0);
    EXPECT_DOUBLE_EQ(s.scales()[0], 1.0);  // sample stddev of 1, 2, 3
    EXPECT_DOUBLE_EQ(s.means()[1], 5.0);
    EXPECT_DOUBLE_EQ(s.scales()[1], 1.0);  // constant column: scale 1, not 0

    const auto z = s.transform({4.0, std::nullopt});
    EXPECT_DOUBLE_EQ(z[0], 2.0);
    EXPECT_DOUBLE_EQ(z[1], 0.0);  // missing maps to the mean
    EXPECT_DOUBLE_EQ(s.transform({1000.0, 5.0})[0], det::Standardizer::kClip);
}

TEST(LearnedTest, KnnScoreIsMeanSquaredDistanceToKNearest) {
    const det::KnnDetector knn{{{0.0, 0.0}, {1.0, 0.0}, {0.0, 2.0}, {10.0, 10.0}}, 2};
    const std::vector<double> origin{0.0, 0.0};
    EXPECT_DOUBLE_EQ(knn.score(origin), 0.5);  // distances^2 0 and 1
    const std::vector<double> far{10.0, 10.0};
    // nearest: itself (0), then (0,2): 100 + 64 = 164
    EXPECT_DOUBLE_EQ(knn.score(far), 82.0);

    // k larger than the reference set is capped; an empty set or k = 0 is refused.
    const det::KnnDetector one{{{3.0}}, 5};
    EXPECT_EQ(one.k(), 1U);
    const std::vector<double> x{1.0};
    EXPECT_DOUBLE_EQ(one.score(x), 4.0);
    EXPECT_THROW(det::KnnDetector({}, 1), std::invalid_argument);
    EXPECT_THROW(det::KnnDetector({{1.0}}, 0), std::invalid_argument);
}

TEST(LearnedTest, KnnEarlyExitMatchesExhaustiveSearch) {
    // Pseudo-random reference set; compare with a plain sort of all distances.
    std::vector<std::vector<double>> ref;
    std::uint64_t state = 42;
    const auto next = [&] {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(state >> 40) / static_cast<double>(1ULL << 24) * 4.0 - 2.0;
    };
    for (int i = 0; i < 300; ++i) {
        ref.push_back({next(), next(), next()});
    }
    const det::KnnDetector knn{ref, 7};
    for (int q = 0; q < 50; ++q) {
        const std::vector<double> z{next(), next(), next()};
        std::vector<double> all;
        for (const auto& r : ref) {
            double d2 = 0.0;
            for (std::size_t i = 0; i < 3; ++i) {
                d2 += (z[i] - r[i]) * (z[i] - r[i]);
            }
            all.push_back(d2);
        }
        std::sort(all.begin(), all.end());
        double sum = 0.0;
        for (std::size_t i = 0; i < 7; ++i) {
            sum += all[i];
        }
        EXPECT_DOUBLE_EQ(knn.score(z), sum / 7.0);
    }
}

TEST(LearnedTest, PerceptronSeparatesLinearlySeparableData) {
    std::vector<std::vector<double>> x;
    std::vector<int> y;
    for (int i = 0; i < 40; ++i) {
        const double a = static_cast<double>(i % 10) / 10.0;
        x.push_back({1.0 + a, a});
        y.push_back(1);
        x.push_back({-1.0 - a, a});
        y.push_back(-1);
    }
    const auto p = det::AveragedPerceptron::train(x, y, {});
    for (std::size_t i = 0; i < x.size(); ++i) {
        EXPECT_EQ(p.score(x[i]) > 0.0, y[i] > 0) << i;
    }
    EXPECT_GT(p.weights()[0], 0.0);
    // Deterministic: the same data in the same order gives the same weights.
    const auto again = det::AveragedPerceptron::train(x, y, {});
    EXPECT_EQ(again.weights(), p.weights());
    EXPECT_EQ(again.bias(), p.bias());
    EXPECT_THROW((void)det::AveragedPerceptron::train(x, {1}, {}), std::invalid_argument);
}

TEST(LearnedTest, AlerterSustainsClearsAndCoolsDown) {
    det::AlerterConfig config;
    config.detector = "knn_distance";
    config.trigger = 10.0;
    config.clear = 5.0;
    config.sustain = seconds{2};
    config.clear_for = seconds{2};
    config.cooldown = seconds{10};
    det::ScoreAlerter alerter{config};
    const auto at = [](int s) { return kT0 + seconds{s}; };
    // 7 (hysteresis band) keeps the alert firing; 4 for 2 s resolves it.
    const std::vector<double> scores{12, 12, 12, 7,  4,  4,  4,  12, 12, 12,
                                     12, 12, 12, 12, 12, 12, 12, 12, 12, 12};
    std::vector<std::pair<int, det::DetectionKind>> events;
    for (int t = 0; t < static_cast<int>(scores.size()); ++t) {
        if (auto d = alerter.step("n", at(t), scores[static_cast<std::size_t>(t)], kNoEvidence)) {
            EXPECT_EQ(d->rule_id, "knn_distance");
            EXPECT_EQ(d->rule_set_version, std::string{det::kLearnedDetectorsVersion});
            events.emplace_back(t, d->kind);
        }
    }
    // Fired at 2 s (sustain), resolved at 6 s (clear_for), cooldown until 16 s.
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events[0], std::make_pair(2, det::DetectionKind::kFired));
    EXPECT_EQ(events[1], std::make_pair(6, det::DetectionKind::kResolved));
    EXPECT_EQ(events[2], std::make_pair(16, det::DetectionKind::kFired));
}

TEST(LearnedTest, LabelsLeaveTheRecoveryTailOut) {
    det::Scenario s;
    s.episodes.push_back(
        {"r1", det::FaultKind::kCpuSaturation, kT0 + seconds{100}, kT0 + seconds{200}, 1.0});
    ft::FeatureVector v;
    v.node = "r1";
    const det::MatchConfig match;
    v.as_of = kT0 + seconds{150};
    EXPECT_EQ(det::label_of(s, v, match), det::VectorLabel::kFault);
    v.as_of = kT0 + seconds{220};
    EXPECT_EQ(det::label_of(s, v, match), det::VectorLabel::kUnlabelled);
    v.as_of = kT0 + seconds{231};
    EXPECT_EQ(det::label_of(s, v, match), det::VectorLabel::kNormal);
    v.node = "r2";
    v.as_of = kT0 + seconds{150};
    EXPECT_EQ(det::label_of(s, v, match), det::VectorLabel::kNormal);
}

struct Split {
    std::vector<det::Scenario> scenarios;
    std::vector<det::ScenarioFeatures> features;
};

Split make_split(std::uint64_t first, std::uint64_t last) {
    Split split;
    const auto& schema = ft::default_feature_schema();
    for (std::uint64_t seed = first; seed <= last; ++seed) {
        det::ScenarioConfig config;
        config.seed = seed;
        config.duration = seconds{300};
        config.faults = 2;
        config.distractors = 2;
        split.scenarios.push_back(det::generate_scenario(config));
        split.features.push_back(det::extract_scenario_features(split.scenarios.back(), schema));
    }
    return split;
}

TEST(LearnedTest, TrainAndTuneIsDeterministicAndUsesOnlyItsInputs) {
    const auto& schema = ft::default_feature_schema();
    const auto train = make_split(1, 2);
    const auto tune = make_split(3, 4);
    det::LearnedConfig config;
    config.knn_reference_capacity = 200;
    config.perceptron_epochs = 3;
    const det::MatchConfig match;
    for (const auto kind : {det::LearnedKind::kKnn, det::LearnedKind::kPerceptron}) {
        const auto a = det::train_and_tune(kind, train.scenarios, train.features, tune.scenarios,
                                           tune.features, schema, config, match);
        const auto b = det::train_and_tune(kind, train.scenarios, train.features, tune.scenarios,
                                           tune.features, schema, config, match);
        ASSERT_EQ(a.grid.size(), config.quantiles.size() * config.sustains.size());
        EXPECT_EQ(a.model.alerter.trigger, b.model.alerter.trigger);
        EXPECT_EQ(a.model.alerter.sustain, b.model.alerter.sustain);
        EXPECT_LE(a.model.alerter.clear, a.model.alerter.trigger);
        EXPECT_EQ(a.model.alerter.detector, std::string{det::detector_id(kind)});
        std::size_t chosen = 0;
        for (const auto& row : a.grid) {
            chosen += row.chosen ? 1U : 0U;
        }
        EXPECT_EQ(chosen, 1U);
        if (kind == det::LearnedKind::kKnn) {
            ASSERT_TRUE(a.model.knn);
            EXPECT_EQ(a.model.knn->reference_size(), 200U);
        } else {
            ASSERT_TRUE(a.model.perceptron);
            EXPECT_GT(a.model.positive_vectors, 0U);
            EXPECT_EQ(a.model.perceptron->weights(), b.model.perceptron->weights());
        }

        // Running on an unseen scenario gives the same detections twice, with evidence.
        const auto test = make_split(1001, 1001);
        const auto d1 = det::run_learned(a.model, test.features[0], schema);
        const auto d2 = det::run_learned(a.model, test.features[0], schema);
        ASSERT_EQ(d1.size(), d2.size());
        for (std::size_t i = 0; i < d1.size(); ++i) {
            EXPECT_EQ(d1[i].at, d2[i].at);
            EXPECT_EQ(d1[i].node, d2[i].node);
            ASSERT_EQ(d1[i].evidence.size(), 4U);
            EXPECT_EQ(d1[i].evidence[0].feature, a.model.alerter.detector + ".score");
        }
        EXPECT_EQ(det::learned_scores(a.model, test.features[0], schema).size(),
                  test.features[0].vectors.size());
    }
}

}  // namespace
