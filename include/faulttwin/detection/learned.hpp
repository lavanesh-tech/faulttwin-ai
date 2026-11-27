#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "faulttwin/detection/evaluation.hpp"
#include "faulttwin/detection/rule_engine.hpp"
#include "faulttwin/detection/scenario.hpp"
#include "faulttwin/features/feature_schema.hpp"

/// Learned comparison detectors (Phase 4, ADR-0018): a k-nearest-neighbour distance detector
/// (unsupervised: it only sees normal vectors) and an averaged perceptron (supervised: it sees
/// labelled vectors). Both are small, native and deterministic, and both run on the same
/// feature vectors, alert state machine and scoring as the rules, so the four detectors can be
/// compared on the same held-out splits. They are not the Phase 7 models (ADR-0008).
namespace faulttwin::detection {

inline constexpr std::string_view kLearnedDetectorsVersion = "faulttwin.learned.v1";

enum class LearnedKind : std::uint8_t { kKnn, kPerceptron };
[[nodiscard]] std::string_view to_string(LearnedKind kind) noexcept;
/// Detector id used in Detection::rule_id: "knn_distance" or "perceptron".
[[nodiscard]] std::string_view detector_id(LearnedKind kind) noexcept;

/// The fixed subset of schema features the learned detectors read.
[[nodiscard]] const std::vector<features::FeatureRef>& learned_features();

/// Picks learned_features() out of a FeatureVector. Throws std::invalid_argument if the schema
/// lacks one of them.
class Projection {
  public:
    explicit Projection(const features::FeatureSchema& schema);
    [[nodiscard]] std::size_t dims() const noexcept { return columns_.size(); }
    [[nodiscard]] std::vector<std::optional<double>> raw(const features::FeatureVector& v) const;

  private:
    std::vector<std::size_t> columns_;
};

/// Per-feature z-score fitted on training rows (present values only). Missing values map to
/// 0 (the training mean); z is clipped to +-kClip so that one extreme value (for example a
/// long telemetry age) cannot dominate a distance or a dot product.
class Standardizer {
  public:
    static constexpr double kClip = 10.0;

    Standardizer() = default;
    [[nodiscard]] static Standardizer fit(
        const std::vector<std::vector<std::optional<double>>>& rows, std::size_t dims);
    [[nodiscard]] std::vector<double> transform(
        const std::vector<std::optional<double>>& raw) const;
    [[nodiscard]] const std::vector<double>& means() const noexcept { return means_; }
    [[nodiscard]] const std::vector<double>& scales() const noexcept { return scales_; }

  private:
    std::vector<double> means_;
    std::vector<double> scales_;  ///< sample stddev, or 1 when undefined or 0
};

/// Score = mean squared Euclidean distance to the k nearest reference vectors. Brute force with
/// early exit once a partial distance exceeds the current k-th best, which does not change the
/// result. No square root, so the score is exact IEEE arithmetic (+, -, *, /).
class KnnDetector {
  public:
    KnnDetector(std::vector<std::vector<double>> reference, std::size_t k);
    [[nodiscard]] double score(std::span<const double> z) const;
    [[nodiscard]] std::size_t k() const noexcept { return k_; }
    [[nodiscard]] std::size_t reference_size() const noexcept { return reference_.size(); }

  private:
    std::vector<std::vector<double>> reference_;
    std::size_t k_;
};

/// Averaged perceptron (Freund & Schapire): mistake-driven updates w += y x, b += y, in a
/// fixed data order for a fixed number of epochs; the returned weights are the average over
/// all steps, which is more stable than the last iterate. Score = w . z + b.
class AveragedPerceptron {
  public:
    struct Config {
        std::size_t epochs{10};
    };
    [[nodiscard]] static AveragedPerceptron train(const std::vector<std::vector<double>>& x,
                                                  const std::vector<int>& y,  ///< +1 / -1
                                                  const Config& config);
    [[nodiscard]] double score(std::span<const double> z) const;
    [[nodiscard]] const std::vector<double>& weights() const noexcept { return weights_; }
    [[nodiscard]] double bias() const noexcept { return bias_; }
    [[nodiscard]] std::size_t mistakes() const noexcept { return mistakes_; }

  private:
    std::vector<double> weights_;
    double bias_{0.0};
    std::size_t mistakes_{0};
};

/// Turns a per-node score stream into alerts with the rules' semantics: fire after the score
/// stays >= trigger for `sustain`, resolve after it stays < clear for `clear_for`, then hold
/// off re-firing for `cooldown`. Severity is always warning.
struct AlerterConfig {
    std::string detector;
    double trigger{0.0};
    double clear{0.0};  ///< <= trigger (hysteresis)
    std::chrono::milliseconds sustain{0};
    std::chrono::milliseconds clear_for{5000};
    std::chrono::milliseconds cooldown{30000};
};

class ScoreAlerter {
  public:
    explicit ScoreAlerter(AlerterConfig config) : config_(std::move(config)) {}
    /// Returns a state change, if any; `evidence` is only called when one happens.
    [[nodiscard]] std::optional<Detection> step(
        const std::string& node, Timestamp at, double score,
        const std::function<std::vector<Evidence>()>& evidence);

  private:
    enum class Phase : std::uint8_t { kInactive, kPending, kFiring, kRecovering };
    struct State {
        Phase phase{Phase::kInactive};
        Timestamp since;
        Timestamp recovering_since;
        std::optional<Timestamp> cooldown_until;
    };
    AlerterConfig config_;
    std::map<std::string, State, std::less<>> nodes_;
};

/// A trained detector plus its tuned alerter.
struct LearnedModel {
    LearnedKind kind{LearnedKind::kKnn};
    Standardizer standardizer;
    std::optional<KnnDetector> knn;
    std::optional<AveragedPerceptron> perceptron;
    AlerterConfig alerter;
    std::size_t training_vectors{0};  ///< rows the standardizer / perceptron saw
    std::size_t positive_vectors{0};  ///< perceptron: rows labelled +1
    double normal_median_score{0.0};  ///< median score of normal tuning vectors

    [[nodiscard]] double score(std::span<const double> z) const;
};

struct LearnedConfig {
    std::size_t knn_k{5};
    std::size_t knn_reference_capacity{1000};
    std::size_t perceptron_epochs{10};
    /// clear = trigger - hysteresis * (trigger - median normal score).
    double hysteresis{0.5};
    std::chrono::milliseconds clear_for{5000};
    std::chrono::milliseconds cooldown{30000};
    std::vector<double> quantiles{0.9, 0.95, 0.98, 0.99, 0.995, 0.998, 0.999, 0.9995};
    std::vector<std::chrono::milliseconds> sustains{
        std::chrono::seconds{0}, std::chrono::seconds{2}, std::chrono::seconds{5},
        std::chrono::seconds{10}, std::chrono::seconds{15}};
};

struct LearnedGridRow {
    double quantile{0.0};
    double trigger{0.0};
    std::chrono::milliseconds sustain{0};
    std::size_t matched{0};
    std::size_t false_alerts{0};
    std::size_t episodes{0};
    std::size_t detected{0};
    DetectionMetrics metrics;
    bool chosen{false};
};

struct LearnedTuning {
    LearnedModel model;
    std::vector<LearnedGridRow> grid;
};

/// Labels used for training: +1 inside [onset, end] of an episode on the vector's node, -1
/// outside every [onset, end + grace] on that node; vectors in the recovery tail
/// (end, end + grace] get no label and are left out of training.
enum class VectorLabel : std::int8_t { kNormal = -1, kUnlabelled = 0, kFault = 1 };
[[nodiscard]] VectorLabel label_of(const Scenario& scenario, const features::FeatureVector& v,
                                   const MatchConfig& match);

/// Fits the detector on `train` (k-NN: normal vectors only, deterministically subsampled to
/// knn_reference_capacity; perceptron: all labelled vectors), then chooses the alert trigger
/// (a quantile of normal-vector scores on `tune`) and sustain on `tune` only, with the rules'
/// objective: highest F1, then fewer false alerts, then lower median latency, then grid order.
[[nodiscard]] LearnedTuning train_and_tune(LearnedKind kind, const std::vector<Scenario>& train,
                                           const std::vector<ScenarioFeatures>& train_features,
                                           const std::vector<Scenario>& tune,
                                           const std::vector<ScenarioFeatures>& tune_features,
                                           const features::FeatureSchema& schema,
                                           const LearnedConfig& config, const MatchConfig& match);

/// Runs a model over a scenario's vectors (in their order) and returns every detection.
[[nodiscard]] std::vector<Detection> run_learned(const LearnedModel& model,
                                                 const ScenarioFeatures& features,
                                                 const features::FeatureSchema& schema);

/// Scores of every vector, in order (exposed for tests and benchmarks).
[[nodiscard]] std::vector<double> learned_scores(const LearnedModel& model,
                                                 const ScenarioFeatures& features,
                                                 const features::FeatureSchema& schema);

}  // namespace faulttwin::detection
