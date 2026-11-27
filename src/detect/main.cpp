// faulttwin-detect: tunes and evaluates the Phase 4 rule baseline and the learned comparison
// detectors (k-NN distance, averaged perceptron) on synthetic scenarios.

#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "detect_options.hpp"
#include "detect_report.hpp"
#include "faulttwin/detection/evaluation.hpp"
#include "faulttwin/detection/learned.hpp"
#include "faulttwin/detection/report.hpp"
#include "faulttwin/detection/scenario.hpp"
#include "faulttwin/features/feature_schema.hpp"

namespace {

namespace det = faulttwin::detection;
namespace dt = faulttwin::detect;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitInternalError = 3;

det::ScenarioConfig scenario_config(std::uint64_t seed, bool shifted) {
    det::ScenarioConfig config;
    config.seed = seed;
    if (shifted) {
        config.intensity_min = 0.5;
        config.intensity_max = 0.8;
    }
    return config;
}

struct Detectors {
    const det::TuningResult* tuned;
    const det::TuningResult* naive;
    const det::LearnedModel* knn;
    const det::LearnedModel* perceptron;
};

dt::SplitResult evaluate_split(std::string name, const dt::SeedRange& seeds, bool shifted,
                               const Detectors& detectors, const det::MatchConfig& match) {
    const auto& tuned = *detectors.tuned;
    const auto& naive = *detectors.naive;
    const auto& schema = faulttwin::features::default_feature_schema();
    const det::RuleSet tuned_rules = det::default_rule_set(tuned.params);
    const det::RuleSet naive_rules = det::default_rule_set(naive.params);
    dt::SplitResult result;
    result.name = std::move(name);
    result.seeds = seeds;
    result.shifted = shifted;
    for (std::uint64_t seed = seeds.first; seed <= seeds.last; ++seed) {
        const det::Scenario scenario = det::generate_scenario(scenario_config(seed, shifted));
        const det::ScenarioFeatures features = det::extract_scenario_features(scenario, schema);
        const auto tuned_detections = det::run_rules(features, tuned_rules, schema);
        const auto naive_detections = det::run_rules(features, naive_rules, schema);
        result.tuned.merge(det::score(scenario, tuned_detections, match));
        result.naive.merge(det::score(scenario, naive_detections, match));
        const auto knn_detections = det::run_learned(*detectors.knn, features, schema);
        const auto perceptron_detections =
            det::run_learned(*detectors.perceptron, features, schema);
        result.knn.merge(det::score(scenario, knn_detections, match));
        result.perceptron.merge(det::score(scenario, perceptron_detections, match));
        dt::append_alert_rows(result.alerts, scenario, tuned_detections, match);
        dt::append_alert_rows(result.alerts, scenario, knn_detections, match);
        dt::append_alert_rows(result.alerts, scenario, perceptron_detections, match);
        result.observations += scenario.observations.size();
        result.vectors += features.vectors.size();
        result.distractors += scenario.distractors.size();
    }
    return result;
}

int run_evaluate(const dt::EvaluateOptions& options) {
    const auto& schema = faulttwin::features::default_feature_schema();
    det::MatchConfig match;
    match.grace = std::chrono::seconds{options.grace_seconds};

    std::vector<det::Scenario> validation;
    std::vector<det::ScenarioFeatures> validation_features;
    validation.reserve(options.validation.size());
    for (std::uint64_t seed = options.validation.first; seed <= options.validation.last; ++seed) {
        validation.push_back(det::generate_scenario(scenario_config(seed, false)));
        validation_features.push_back(det::extract_scenario_features(validation.back(), schema));
    }
    const det::TuningResult tuned = det::tune_default_rules(validation, validation_features, schema,
                                                            det::BaselineKind::kTunedRules, match);
    const det::TuningResult naive = det::tune_default_rules(
        validation, validation_features, schema, det::BaselineKind::kNaiveThreshold, match);

    dt::EvaluationReport report;
    report.options = options;
    report.tuned = tuned;
    report.naive = naive;
    report.validation_episodes = 0;
    for (const auto& s : validation) {
        report.validation_episodes += s.episodes.size();
    }

    // Learned detectors: fit on the first half of the validation seeds, choose the alert
    // threshold on the second half. Neither half overlaps a test split.
    const std::size_t half = validation.size() / 2;
    report.learned_train = {options.validation.first, options.validation.first + half - 1};
    report.learned_tune = {options.validation.first + half, options.validation.last};
    const auto first_half = [&](const auto& all) {
        using T = std::decay_t<decltype(all)>;
        return T(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(half));
    };
    const auto second_half = [&](const auto& all) {
        using T = std::decay_t<decltype(all)>;
        return T(all.begin() + static_cast<std::ptrdiff_t>(half), all.end());
    };
    const auto train = first_half(validation);
    const auto train_features = first_half(validation_features);
    const auto tune = second_half(validation);
    const auto tune_features = second_half(validation_features);
    validation.clear();
    validation_features.clear();
    const det::LearnedConfig learned_config;
    report.knn = det::train_and_tune(det::LearnedKind::kKnn, train, train_features, tune,
                                     tune_features, schema, learned_config, match);
    report.perceptron = det::train_and_tune(det::LearnedKind::kPerceptron, train, train_features,
                                            tune, tune_features, schema, learned_config, match);

    const Detectors detectors{&tuned, &naive, &report.knn.model, &report.perceptron.model};
    report.splits.push_back(evaluate_split("test", options.test, false, detectors, match));
    if (options.shifted) {
        report.splits.push_back(
            evaluate_split("test_shifted", *options.shifted, true, detectors, match));
    }

    std::cout << dt::summary_markdown(report);
    if (!options.out_dir.empty()) {
        const std::filesystem::path dir{options.out_dir};
        std::filesystem::create_directories(dir);
        const auto write = [&](const std::string& name, const std::string& text) {
            std::ofstream out{dir / name, std::ios::binary};
            out << text;
            if (!out) {
                throw std::runtime_error("cannot write " + (dir / name).string());
            }
        };
        write("summary.json", dt::summary_json(report));
        write("summary.md", dt::summary_markdown(report));
        write("tuning-tuned.csv", dt::tuning_csv(tuned));
        write("tuning-naive.csv", dt::tuning_csv(naive));
        write("tuning-knn.csv", dt::learned_tuning_csv(report.knn));
        write("tuning-perceptron.csv", dt::learned_tuning_csv(report.perceptron));
        for (const auto& split : report.splits) {
            write("alerts-" + split.name + ".csv", dt::alerts_csv(split.alerts));
        }
    }
    return kExitOk;
}

int run_scenario(const dt::ScenarioOptions& options) {
    const auto& schema = faulttwin::features::default_feature_schema();
    const det::Scenario scenario = det::generate_scenario(scenario_config(options.seed, false));
    const auto features = det::extract_scenario_features(scenario, schema);
    const auto detections = det::run_rules(features, det::default_rule_set(), schema);
    std::cout << dt::scenario_text(scenario, detections, options.jsonl);
    return kExitOk;
}

int run(const std::vector<std::string_view>& args) {
    const auto command = dt::parse_detect_options(args);
    if (std::holds_alternative<dt::HelpRequested>(command)) {
        std::cout << dt::detect_usage(args.empty() ? "faulttwin-detect" : args[0]);
        return kExitOk;
    }
    if (const auto* error = std::get_if<dt::UsageError>(&command)) {
        std::cerr << "error: " << error->message << "\n\n"
                  << dt::detect_usage(args.empty() ? "faulttwin-detect" : args[0]);
        return kExitUsage;
    }
    if (const auto* evaluate = std::get_if<dt::EvaluateOptions>(&command)) {
        return run_evaluate(*evaluate);
    }
    return run_scenario(std::get<dt::ScenarioOptions>(command));
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::vector<std::string_view> args(argv, argv + argc);
        return run(args);
    } catch (const std::exception& e) {
        std::cerr << "internal error: " << e.what() << '\n';
    } catch (...) {
        std::cerr << "internal error: unknown exception\n";
    }
    return kExitInternalError;
}
