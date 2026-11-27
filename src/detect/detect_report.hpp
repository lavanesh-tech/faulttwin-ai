#pragma once

#include <string>
#include <vector>

#include "detect_options.hpp"
#include "faulttwin/detection/evaluation.hpp"
#include "faulttwin/detection/learned.hpp"

namespace faulttwin::detect {

/// One alert (kFired detection) of the tuned rules or a learned detector, and what it matched.
struct AlertRow {
    std::uint64_t seed{0};
    std::string rule_id;
    std::string node;
    std::string severity;
    double at_seconds{0.0};  ///< since scenario start
    std::string matched;     ///< fault kind of the matched episode, or "false_alert"
};

struct SplitResult {
    std::string name;
    SeedRange seeds;
    bool shifted{false};
    detection::EvaluationCounts tuned;
    detection::EvaluationCounts naive;
    detection::EvaluationCounts knn;
    detection::EvaluationCounts perceptron;
    std::vector<AlertRow> alerts;
    std::size_t observations{0};
    std::size_t vectors{0};
    std::size_t distractors{0};
};

struct EvaluationReport {
    EvaluateOptions options;
    detection::TuningResult tuned;
    detection::TuningResult naive;
    detection::LearnedTuning knn;
    detection::LearnedTuning perceptron;
    SeedRange learned_train;  ///< first half of the validation seeds
    SeedRange learned_tune;   ///< second half
    std::size_t validation_episodes{0};
    std::vector<SplitResult> splits;
};

void append_alert_rows(std::vector<AlertRow>& rows, const detection::Scenario& scenario,
                       const std::vector<detection::Detection>& detections,
                       const detection::MatchConfig& match);

[[nodiscard]] std::string summary_markdown(const EvaluationReport& report);
[[nodiscard]] std::string summary_json(const EvaluationReport& report);
[[nodiscard]] std::string tuning_csv(const detection::TuningResult& tuning);
[[nodiscard]] std::string learned_tuning_csv(const detection::LearnedTuning& tuning);
[[nodiscard]] std::string alerts_csv(const std::vector<AlertRow>& rows);
[[nodiscard]] std::string scenario_text(const detection::Scenario& scenario,
                                        const std::vector<detection::Detection>& detections,
                                        bool jsonl);

}  // namespace faulttwin::detect
