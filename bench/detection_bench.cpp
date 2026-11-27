// Phase 4 micro-benchmarks: rolling-window statistics, feature extraction, rule evaluation
// the learned comparison detectors and the end-to-end scenario path. Release builds only (ADR-0011,
// ADR-0013).

#include <benchmark/benchmark.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "faulttwin/detection/evaluation.hpp"
#include "faulttwin/detection/learned.hpp"
#include "faulttwin/detection/rule.hpp"
#include "faulttwin/detection/rule_engine.hpp"
#include "faulttwin/detection/scenario.hpp"
#include "faulttwin/features/feature_extractor.hpp"
#include "faulttwin/features/feature_schema.hpp"
#include "faulttwin/features/rolling_window.hpp"

namespace {

namespace det = faulttwin::detection;
namespace ft = faulttwin::features;
using std::chrono::milliseconds;

const ft::Timestamp kT0{std::chrono::sys_days{std::chrono::year{2026} / 1 / 1}};

double value_at(std::int64_t i) {
    return 50.0 + static_cast<double>((i * 7919) % 1000) / 100.0;
}

/// Steady-state push into a full window: one append + one span eviction per push.
void BM_RollingWindowPush(benchmark::State& state) {
    const auto capacity = static_cast<std::size_t>(state.range(0));
    ft::WindowConfig config;
    config.capacity = capacity;
    config.span = milliseconds{static_cast<std::int64_t>(capacity) * 1000};
    ft::RollingWindow window{config};
    std::int64_t i = 0;
    for (; i < static_cast<std::int64_t>(capacity); ++i) {
        (void)window.push(kT0 + milliseconds{i * 1000}, value_at(i), true);
    }
    for (auto _ : state) {
        benchmark::DoNotOptimize(window.push(kT0 + milliseconds{i * 1000}, value_at(i), true));
        ++i;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RollingWindowPush)->Arg(60)->Arg(256);

/// stats() over a full window (O(n): min, max, rate, slope, missing ratio).
void BM_RollingWindowStats(benchmark::State& state) {
    const auto capacity = static_cast<std::size_t>(state.range(0));
    ft::WindowConfig config;
    config.capacity = capacity;
    config.span = milliseconds{static_cast<std::int64_t>(capacity) * 1000};
    ft::RollingWindow window{config};
    const auto n = static_cast<std::int64_t>(capacity);
    for (std::int64_t i = 0; i < n; ++i) {
        (void)window.push(kT0 + milliseconds{i * 1000}, value_at(i), i % 10 != 0);
    }
    const auto now = kT0 + milliseconds{(n - 1) * 1000};
    for (auto _ : state) {
        benchmark::DoNotOptimize(window.stats(now));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RollingWindowStats)->Arg(60)->Arg(256);

/// One FeatureVector (90 features, 10 metrics x 60 s windows at 1 s) for one node.
void BM_FeatureExtract(benchmark::State& state) {
    ft::FeatureExtractor extractor{ft::default_feature_schema()};
    std::int64_t t = 0;
    for (; t < 60; ++t) {
        for (const ft::MetricId m : ft::kAllMetrics) {
            (void)extractor.observe(ft::Observation::ok("node", m, kT0 + milliseconds{t * 1000},
                                                        value_at(t + static_cast<int>(m))));
        }
    }
    const auto now = kT0 + milliseconds{(t - 1) * 1000};
    for (auto _ : state) {
        benchmark::DoNotOptimize(extractor.extract("node", now));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FeatureExtract);

/// One evaluation of the 5 default rules for one node.
void BM_RuleEngineEvaluate(benchmark::State& state) {
    const auto& schema = ft::default_feature_schema();
    det::RuleEngine engine{det::default_rule_set(), schema};
    ft::FeatureVector v;
    v.schema_version = schema.version();
    v.schema_fingerprint = schema.fingerprint();
    v.node = "node";
    v.values.assign(schema.size(), 1.0);
    std::int64_t t = 0;
    for (auto _ : state) {
        v.as_of = kT0 + milliseconds{t++ * 1000};
        benchmark::DoNotOptimize(engine.evaluate(v));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RuleEngineEvaluate);

/// Learned detectors trained once (seeds 1-2 train, 3-4 tune) for the scoring benchmarks.
const det::LearnedModel& learned_model(det::LearnedKind kind) {
    static const auto build = [](det::LearnedKind k) {
        const auto& schema = ft::default_feature_schema();
        std::vector<det::Scenario> scenarios;
        std::vector<det::ScenarioFeatures> features;
        for (std::uint64_t seed = 1; seed <= 4; ++seed) {
            det::ScenarioConfig config;
            config.seed = seed;
            scenarios.push_back(det::generate_scenario(config));
            features.push_back(det::extract_scenario_features(scenarios.back(), schema));
        }
        const std::vector<det::Scenario> train(scenarios.begin(), scenarios.begin() + 2);
        const std::vector<det::Scenario> tune(scenarios.begin() + 2, scenarios.end());
        const std::vector<det::ScenarioFeatures> train_f(features.begin(), features.begin() + 2);
        const std::vector<det::ScenarioFeatures> tune_f(features.begin() + 2, features.end());
        return det::train_and_tune(k, train, train_f, tune, tune_f, schema, det::LearnedConfig{},
                                   det::MatchConfig{})
            .model;
    };
    static const det::LearnedModel knn = build(det::LearnedKind::kKnn);
    static const det::LearnedModel perceptron = build(det::LearnedKind::kPerceptron);
    return kind == det::LearnedKind::kKnn ? knn : perceptron;
}

/// Scores one standardized vector. k-NN: brute force over the 1,000-vector reference set with
/// early exit; perceptron: one 8-wide dot product. Arg 0 = k-NN, 1 = perceptron.
void BM_LearnedScore(benchmark::State& state) {
    const auto kind = state.range(0) == 0 ? det::LearnedKind::kKnn : det::LearnedKind::kPerceptron;
    const auto& model = learned_model(kind);
    det::ScenarioConfig config;
    config.seed = 9;
    const auto scenario = det::generate_scenario(config);
    const auto& schema = ft::default_feature_schema();
    const auto features = det::extract_scenario_features(scenario, schema);
    const det::Projection projection{schema};
    std::vector<std::vector<double>> rows;
    rows.reserve(features.vectors.size());
    for (const auto& v : features.vectors) {
        rows.push_back(model.standardizer.transform(projection.raw(v)));
    }
    std::size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(model.score(rows[i]));
        i = (i + 1) % rows.size();
    }
    state.SetItemsProcessed(state.iterations());
    state.SetLabel(std::string{det::to_string(kind)});
}
BENCHMARK(BM_LearnedScore)->Arg(0)->Arg(1);

/// End to end on one 900 s scenario (3 nodes, ~24k observations, 2,700 vectors): feature
/// extraction + default rules. Items = feature vectors evaluated.
void BM_ScenarioEndToEnd(benchmark::State& state) {
    det::ScenarioConfig config;
    config.seed = 1;
    const auto scenario = det::generate_scenario(config);
    const auto& schema = ft::default_feature_schema();
    const auto rules = det::default_rule_set();
    std::size_t vectors = 0;
    for (auto _ : state) {
        const auto features = det::extract_scenario_features(scenario, schema);
        benchmark::DoNotOptimize(det::run_rules(features, rules, schema));
        vectors += features.vectors.size();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(vectors));
    state.counters["observations"] = static_cast<double>(scenario.observations.size());
}
BENCHMARK(BM_ScenarioEndToEnd)->Unit(benchmark::kMillisecond);

}  // namespace

BENCHMARK_MAIN();
