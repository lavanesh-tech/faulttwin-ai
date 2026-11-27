#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "faulttwin/detection/rule.hpp"
#include "faulttwin/detection/rule_engine.hpp"
#include "faulttwin/detection/scenario.hpp"
#include "faulttwin/features/feature_extractor.hpp"

namespace faulttwin::detection {

/// Feature vectors of every node at every tick of a scenario, in (tick, node) order. The
/// scenario's observations are fed tick by tick, so each vector only sees data up to its
/// as_of (no look-ahead).
struct ScenarioFeatures {
    std::vector<features::FeatureVector> vectors;
    features::ExtractorCounters counters;
};
[[nodiscard]] ScenarioFeatures extract_scenario_features(const Scenario& scenario,
                                                         const features::FeatureSchema& schema);

/// Runs a rule set over precomputed vectors and returns every detection in order.
[[nodiscard]] std::vector<Detection> run_rules(const ScenarioFeatures& features,
                                               const RuleSet& rules,
                                               const features::FeatureSchema& schema);

/// Matching of alerts (kFired detections) to ground-truth episodes, the same for rules and
/// (Phase 7) the model: an alert on node N at time t matches an episode on N when
/// onset <= t <= end + grace. Alerts that match no episode are false alerts.
struct MatchConfig {
    std::chrono::seconds grace{30};
};

struct KindCounts {
    std::size_t episodes{0};
    std::size_t detected{0};
    std::vector<double> latencies_seconds;  ///< first matching alert - onset, detected only
};

struct RuleCounts {
    std::size_t alerts{0};
    std::size_t matched{0};
    std::size_t false_alerts{0};
};

struct EvaluationCounts {
    std::size_t alerts{0};
    std::size_t matched_alerts{0};
    std::size_t false_alerts{0};
    std::size_t episodes{0};
    std::size_t detected{0};
    double negative_hours{0.0};  ///< node-hours outside every [onset, end + grace]
    std::array<KindCounts, kFaultKindCount> by_kind{};
    std::map<std::string, RuleCounts, std::less<>> by_rule;

    void merge(const EvaluationCounts& other);
};

/// Scores detections. `target` restricts the episodes considered (used when tuning one rule
/// for one fault class): alerts inside a non-target episode are then neither matched nor
/// false, and non-target episodes are not counted.
[[nodiscard]] EvaluationCounts score(const Scenario& scenario,
                                     const std::vector<Detection>& detections,
                                     const MatchConfig& match,
                                     const std::function<bool(FaultKind)>& target = nullptr);

/// DET-* metrics from counts (docs/evaluation/metric-definitions.md §5). Undefined ratios are
/// nullopt (reported as N/A), never 0 or 1.
struct DetectionMetrics {
    std::optional<double> precision;
    std::optional<double> recall;
    std::optional<double> f1;
    std::optional<double> false_alerts_per_hour;
    std::optional<double> missed_rate;
    std::optional<double> latency_median_seconds;
    std::optional<double> latency_p95_seconds;
    std::optional<double> latency_max_seconds;
};
[[nodiscard]] DetectionMetrics metrics(const EvaluationCounts& counts);
[[nodiscard]] DetectionMetrics kind_metrics(const KindCounts& counts);

/// Nearest-rank percentile (metric-definitions.md §1); nullopt for no samples.
[[nodiscard]] std::optional<double> nearest_rank_percentile(std::vector<double> values,
                                                            double percentile);

// ---- tuning ---------------------------------------------------------------------------------

/// One evaluated grid point of one rule on the validation scenarios.
struct GridRow {
    std::string rule_id;
    double trigger{0.0};
    std::chrono::milliseconds sustain{0};
    std::size_t matched{0};
    std::size_t false_alerts{0};
    std::size_t episodes{0};
    std::size_t detected{0};
    DetectionMetrics metrics;
    bool chosen{false};
};

struct TuningResult {
    DefaultRuleParams params;
    std::vector<GridRow> grid;
};

enum class BaselineKind : std::uint8_t {
    kTunedRules,      ///< sustain, hysteresis and cooldown tuned / enabled
    kNaiveThreshold,  ///< same predicates, but no sustain, no hysteresis, no cooldown
};

/// Chooses each default rule's trigger and sustain on the given (validation) scenarios only,
/// one rule at a time against its own fault class. Objective: highest F1; ties broken by
/// fewer false alerts, then lower median latency, then grid order. The naive baseline gets
/// the same treatment with sustain fixed at 0, so both are tuned with equal effort.
[[nodiscard]] TuningResult tune_default_rules(const std::vector<Scenario>& validation,
                                              const std::vector<ScenarioFeatures>& features,
                                              const features::FeatureSchema& schema,
                                              BaselineKind kind, const MatchConfig& match);

/// Parameters of the naive baseline: hysteresis, clear_for and cooldown removed.
[[nodiscard]] DefaultRuleParams naive_params(DefaultRuleParams params);

}  // namespace faulttwin::detection
