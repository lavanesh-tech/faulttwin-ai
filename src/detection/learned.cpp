#include "faulttwin/detection/learned.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace faulttwin::detection {

namespace {

using features::FeatureKind;
using features::FeatureRef;
using features::MetricId;
using std::chrono::milliseconds;

double dot(std::span<const double> a, std::span<const double> b) {
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
        sum += a[i] * b[i];
    }
    return sum;
}

/// Nearest-rank quantile of sorted values (metric-definitions.md §1).
double quantile_of_sorted(const std::vector<double>& sorted, double q) {
    if (sorted.empty()) {
        return 0.0;
    }
    const auto n = static_cast<double>(sorted.size());
    const double rank = std::ceil(q * n);
    const auto index = static_cast<std::size_t>(std::clamp(rank, 1.0, n)) - 1;
    return sorted[index];
}

bool better(const LearnedGridRow& a, const LearnedGridRow& b) {
    const double fa = a.metrics.f1.value_or(-1.0);
    const double fb = b.metrics.f1.value_or(-1.0);
    if (fa != fb) {
        return fa > fb;
    }
    if (a.false_alerts != b.false_alerts) {
        return a.false_alerts < b.false_alerts;
    }
    return a.metrics.latency_median_seconds.value_or(1e300) <
           b.metrics.latency_median_seconds.value_or(1e300);
}

/// Standardized rows of every vector of every scenario, in order.
std::vector<std::vector<std::vector<double>>> standardize_all(
    const Standardizer& standardizer, const Projection& projection,
    const std::vector<ScenarioFeatures>& features) {
    std::vector<std::vector<std::vector<double>>> out;
    out.reserve(features.size());
    for (const auto& f : features) {
        std::vector<std::vector<double>> rows;
        rows.reserve(f.vectors.size());
        for (const auto& v : f.vectors) {
            rows.push_back(standardizer.transform(projection.raw(v)));
        }
        out.push_back(std::move(rows));
    }
    return out;
}

std::vector<Detection> alert_over(const AlerterConfig& config, const ScenarioFeatures& features,
                                  const std::vector<double>& scores) {
    ScoreAlerter alerter{config};
    std::vector<Detection> out;
    const std::function<std::vector<Evidence>()> no_evidence = [] {
        return std::vector<Evidence>{};
    };
    for (std::size_t i = 0; i < features.vectors.size() && i < scores.size(); ++i) {
        const auto& v = features.vectors[i];
        if (auto d = alerter.step(v.node, v.as_of, scores[i], no_evidence)) {
            out.push_back(std::move(*d));
        }
    }
    return out;
}

void fit_model(LearnedModel& model, const std::vector<std::vector<double>>& normal,
               const std::vector<std::vector<double>>& labelled, const std::vector<int>& labels,
               const LearnedConfig& config) {
    if (model.kind == LearnedKind::kKnn) {
        if (normal.empty()) {
            throw std::invalid_argument("k-NN: no normal training vectors");
        }
        // Deterministic, evenly spaced subsample in data order.
        const std::size_t n = normal.size();
        const std::size_t cap = std::max<std::size_t>(1, config.knn_reference_capacity);
        std::vector<std::vector<double>> reference;
        if (n <= cap) {
            reference = normal;
        } else {
            reference.reserve(cap);
            for (std::size_t i = 0; i < cap; ++i) {
                reference.push_back(normal[i * n / cap]);
            }
        }
        model.knn.emplace(std::move(reference), config.knn_k);
        model.training_vectors = normal.size();
    } else {
        AveragedPerceptron::Config pc;
        pc.epochs = config.perceptron_epochs;
        model.perceptron = AveragedPerceptron::train(labelled, labels, pc);
        model.training_vectors = labelled.size();
        model.positive_vectors =
            static_cast<std::size_t>(std::count(labels.begin(), labels.end(), 1));
    }
}

}  // namespace

std::string_view to_string(LearnedKind kind) noexcept {
    return kind == LearnedKind::kKnn ? "knn" : "perceptron";
}

std::string_view detector_id(LearnedKind kind) noexcept {
    return kind == LearnedKind::kKnn ? "knn_distance" : "perceptron";
}

const std::vector<FeatureRef>& learned_features() {
    static const std::vector<FeatureRef> refs{
        {MetricId::kReplicationLagSeconds, FeatureKind::kLast},
        {MetricId::kReplicationLagSeconds, FeatureKind::kSlopePerSecond},
        {MetricId::kCpuPercent, FeatureKind::kLast},
        {MetricId::kCpuPercent, FeatureKind::kMean},
        {MetricId::kStatementLatencyMs, FeatureKind::kLast},
        {MetricId::kConnectionUtilization, FeatureKind::kLast},
        {MetricId::kAvailability, FeatureKind::kAgeSeconds},
        {MetricId::kAvailability, FeatureKind::kMissingRatio},
    };
    return refs;
}

// ---- Projection / Standardizer --------------------------------------------------------------

Projection::Projection(const features::FeatureSchema& schema) {
    for (const auto& ref : learned_features()) {
        const auto index = schema.index_of(ref);
        if (!index) {
            throw std::invalid_argument("schema " + schema.version() + " lacks feature " +
                                        features::feature_name(ref));
        }
        columns_.push_back(*index);
    }
}

std::vector<std::optional<double>> Projection::raw(const features::FeatureVector& v) const {
    std::vector<std::optional<double>> out;
    out.reserve(columns_.size());
    for (const std::size_t c : columns_) {
        out.push_back(c < v.values.size() ? v.values[c] : std::nullopt);
    }
    return out;
}

Standardizer Standardizer::fit(const std::vector<std::vector<std::optional<double>>>& rows,
                               std::size_t dims) {
    Standardizer s;
    s.means_.assign(dims, 0.0);
    s.scales_.assign(dims, 1.0);
    for (std::size_t d = 0; d < dims; ++d) {
        // Welford over present values, in row order.
        double n = 0.0;
        double mean = 0.0;
        double m2 = 0.0;
        for (const auto& row : rows) {
            if (d >= row.size()) {
                continue;
            }
            const std::optional<double>& cell = row[d];
            if (!cell.has_value()) {
                continue;
            }
            const double x = cell.value();
            n += 1.0;
            const double delta = x - mean;
            mean += delta / n;
            m2 += delta * (x - mean);
        }
        if (n > 0.0) {
            s.means_[d] = mean;
        }
        if (n > 1.0) {
            const double sd = std::sqrt(m2 / (n - 1.0));
            if (sd > 0.0) {
                s.scales_[d] = sd;
            }
        }
    }
    return s;
}

std::vector<double> Standardizer::transform(const std::vector<std::optional<double>>& raw) const {
    std::vector<double> z(means_.size(), 0.0);
    for (std::size_t d = 0; d < z.size() && d < raw.size(); ++d) {
        const std::optional<double>& cell = raw[d];
        if (cell.has_value()) {
            z[d] = std::clamp((cell.value() - means_[d]) / scales_[d], -kClip, kClip);
        }
    }
    return z;
}

// ---- k-NN -----------------------------------------------------------------------------------

KnnDetector::KnnDetector(std::vector<std::vector<double>> reference, std::size_t k)
    : reference_(std::move(reference)), k_(k) {
    if (reference_.empty() || k_ == 0) {
        throw std::invalid_argument("k-NN needs k >= 1 and a non-empty reference set");
    }
    k_ = std::min(k_, reference_.size());
}

double KnnDetector::score(std::span<const double> z) const {
    // best[0..filled) holds the smallest squared distances seen so far, ascending.
    std::vector<double> best(k_, 0.0);
    std::size_t filled = 0;
    for (const auto& r : reference_) {
        const double bound = filled == k_ ? best[k_ - 1] : 1e300;
        double d2 = 0.0;
        for (std::size_t i = 0; i < r.size() && i < z.size(); ++i) {
            const double diff = z[i] - r[i];
            d2 += diff * diff;
            if (d2 >= bound) {
                break;  // cannot enter the k best; skipping it leaves the result unchanged
            }
        }
        if (d2 >= bound) {
            continue;
        }
        std::size_t pos = filled == k_ ? k_ - 1 : filled++;
        while (pos > 0 && best[pos - 1] > d2) {
            best[pos] = best[pos - 1];
            --pos;
        }
        best[pos] = d2;
    }
    double sum = 0.0;
    for (std::size_t i = 0; i < filled; ++i) {
        sum += best[i];
    }
    return sum / static_cast<double>(filled);
}

// ---- perceptron -----------------------------------------------------------------------------

AveragedPerceptron AveragedPerceptron::train(const std::vector<std::vector<double>>& x,
                                             const std::vector<int>& y, const Config& config) {
    if (x.empty() || x.size() != y.size()) {
        throw std::invalid_argument("perceptron needs as many labels as rows, and >= 1 row");
    }
    const std::size_t dims = x.front().size();
    std::vector<double> w(dims, 0.0);
    std::vector<double> u(dims, 0.0);  // sum of c * y * x over updates
    double b = 0.0;
    double beta = 0.0;
    double c = 1.0;
    AveragedPerceptron p;
    for (std::size_t epoch = 0; epoch < config.epochs; ++epoch) {
        for (std::size_t i = 0; i < x.size(); ++i) {
            const double yi = y[i] > 0 ? 1.0 : -1.0;
            if (yi * (dot(w, x[i]) + b) <= 0.0) {
                for (std::size_t d = 0; d < dims && d < x[i].size(); ++d) {
                    w[d] += yi * x[i][d];
                    u[d] += c * yi * x[i][d];
                }
                b += yi;
                beta += c * yi;
                ++p.mistakes_;
            }
            c += 1.0;
        }
    }
    p.weights_.resize(dims);
    for (std::size_t d = 0; d < dims; ++d) {
        p.weights_[d] = w[d] - u[d] / c;
    }
    p.bias_ = b - beta / c;
    return p;
}

double AveragedPerceptron::score(std::span<const double> z) const {
    return dot(weights_, z) + bias_;
}

// ---- alerter --------------------------------------------------------------------------------

std::optional<Detection> ScoreAlerter::step(
    const std::string& node, Timestamp at, double score,
    const std::function<std::vector<Evidence>()>& evidence) {
    State& s = nodes_[node];
    const auto make = [&](DetectionKind kind) {
        Detection d;
        d.kind = kind;
        d.rule_set_version = std::string{kLearnedDetectorsVersion};
        d.rule_id = config_.detector;
        d.node = node;
        d.severity = Severity::kWarning;
        d.at = at;
        d.violation_since = s.since;
        d.evidence = evidence();
        return d;
    };
    if (s.phase == Phase::kInactive || s.phase == Phase::kPending) {
        if (score < config_.trigger) {
            s.phase = Phase::kInactive;
            return std::nullopt;
        }
        if (s.phase == Phase::kInactive) {
            s.phase = Phase::kPending;
            s.since = at;
        }
        if (at - s.since < config_.sustain || (s.cooldown_until && at < *s.cooldown_until)) {
            return std::nullopt;
        }
        s.phase = Phase::kFiring;
        s.cooldown_until.reset();
        return make(DetectionKind::kFired);
    }
    if (score >= config_.clear) {
        s.phase = Phase::kFiring;
        return std::nullopt;
    }
    if (s.phase == Phase::kFiring) {
        s.phase = Phase::kRecovering;
        s.recovering_since = at;
    }
    if (at - s.recovering_since < config_.clear_for) {
        return std::nullopt;
    }
    s.phase = Phase::kInactive;
    s.cooldown_until = at + config_.cooldown;
    return make(DetectionKind::kResolved);
}

// ---- model, training, tuning ----------------------------------------------------------------

double LearnedModel::score(std::span<const double> z) const {
    if (knn) {
        return knn->score(z);
    }
    if (perceptron) {
        return perceptron->score(z);
    }
    throw std::logic_error("learned model is not trained");
}

VectorLabel label_of(const Scenario& scenario, const features::FeatureVector& v,
                     const MatchConfig& match) {
    VectorLabel label = VectorLabel::kNormal;
    for (const auto& e : scenario.episodes) {
        if (e.node != v.node) {
            continue;
        }
        if (v.as_of >= e.onset && v.as_of <= e.end) {
            return VectorLabel::kFault;
        }
        if (v.as_of > e.end && v.as_of <= e.end + match.grace) {
            label = VectorLabel::kUnlabelled;
        }
    }
    return label;
}

namespace {

/// Step 1 and 2 of training: standardizer on every training vector (labels are not needed
/// for it), then the detector on the labelled ones.
void train_model(LearnedModel& model, const std::vector<Scenario>& train,
                 const std::vector<ScenarioFeatures>& train_features, const Projection& projection,
                 const LearnedConfig& config, const MatchConfig& match) {
    std::vector<std::vector<std::optional<double>>> raw_rows;
    std::vector<VectorLabel> labels;
    for (std::size_t s = 0; s < train.size() && s < train_features.size(); ++s) {
        for (const auto& v : train_features[s].vectors) {
            raw_rows.push_back(projection.raw(v));
            labels.push_back(label_of(train[s], v, match));
        }
    }
    model.standardizer = Standardizer::fit(raw_rows, projection.dims());

    // k-NN keeps normal rows only; the perceptron keeps every labelled row.
    std::vector<std::vector<double>> normal;
    std::vector<std::vector<double>> labelled;
    std::vector<int> y;
    for (std::size_t i = 0; i < raw_rows.size(); ++i) {
        if (labels[i] == VectorLabel::kUnlabelled) {
            continue;
        }
        auto z = model.standardizer.transform(raw_rows[i]);
        if (model.kind == LearnedKind::kPerceptron) {
            y.push_back(labels[i] == VectorLabel::kFault ? 1 : -1);
            labelled.push_back(std::move(z));
        } else if (labels[i] == VectorLabel::kNormal) {
            normal.push_back(std::move(z));
        }
    }
    raw_rows.clear();
    fit_model(model, normal, labelled, y, config);
}

/// Scores of every tuning vector, per scenario, plus the sorted scores of the normal ones.
struct TuneScores {
    std::vector<std::vector<double>> per_scenario;
    std::vector<double> normal_sorted;
};

TuneScores score_tuning(const LearnedModel& model, const std::vector<Scenario>& tune,
                        const std::vector<ScenarioFeatures>& tune_features,
                        const Projection& projection, const MatchConfig& match) {
    TuneScores out;
    const auto rows = standardize_all(model.standardizer, projection, tune_features);
    for (std::size_t s = 0; s < rows.size() && s < tune.size(); ++s) {
        std::vector<double> scores;
        scores.reserve(rows[s].size());
        for (std::size_t i = 0; i < rows[s].size(); ++i) {
            const double value = model.score(rows[s][i]);
            scores.push_back(value);
            if (label_of(tune[s], tune_features[s].vectors[i], match) == VectorLabel::kNormal) {
                out.normal_sorted.push_back(value);
            }
        }
        out.per_scenario.push_back(std::move(scores));
    }
    std::sort(out.normal_sorted.begin(), out.normal_sorted.end());
    return out;
}

void configure(AlerterConfig& a, double trigger, milliseconds sustain, double median,
               const LearnedConfig& config) {
    a.trigger = trigger;
    a.clear = trigger - config.hysteresis * std::max(0.0, trigger - median);
    a.sustain = sustain;
}

LearnedGridRow grid_row(const AlerterConfig& candidate, double quantile,
                        const std::vector<Scenario>& tune,
                        const std::vector<ScenarioFeatures>& tune_features,
                        const TuneScores& scores, const MatchConfig& match) {
    EvaluationCounts total;
    for (std::size_t s = 0; s < scores.per_scenario.size(); ++s) {
        total.merge(
            score(tune[s], alert_over(candidate, tune_features[s], scores.per_scenario[s]), match));
    }
    LearnedGridRow row;
    row.quantile = quantile;
    row.trigger = candidate.trigger;
    row.sustain = candidate.sustain;
    row.matched = total.matched_alerts;
    row.false_alerts = total.false_alerts;
    row.episodes = total.episodes;
    row.detected = total.detected;
    row.metrics = metrics(total);
    return row;
}

}  // namespace

LearnedTuning train_and_tune(LearnedKind kind, const std::vector<Scenario>& train,
                             const std::vector<ScenarioFeatures>& train_features,
                             const std::vector<Scenario>& tune,
                             const std::vector<ScenarioFeatures>& tune_features,
                             const features::FeatureSchema& schema, const LearnedConfig& config,
                             const MatchConfig& match) {
    const Projection projection{schema};
    LearnedTuning result;
    LearnedModel& model = result.model;
    model.kind = kind;
    train_model(model, train, train_features, projection, config, match);

    // Candidate triggers are quantiles of the scores of normal tuning vectors.
    const TuneScores scores = score_tuning(model, tune, tune_features, projection, match);
    model.normal_median_score = quantile_of_sorted(scores.normal_sorted, 0.5);
    model.alerter.detector = std::string{detector_id(kind)};
    model.alerter.clear_for = config.clear_for;
    model.alerter.cooldown = config.cooldown;

    std::optional<std::size_t> best;
    for (const double q : config.quantiles) {
        const double trigger = quantile_of_sorted(scores.normal_sorted, q);
        for (const auto sustain : config.sustains) {
            AlerterConfig candidate = model.alerter;
            configure(candidate, trigger, sustain, model.normal_median_score, config);
            result.grid.push_back(grid_row(candidate, q, tune, tune_features, scores, match));
            if (!best || better(result.grid.back(), result.grid[*best])) {
                best = result.grid.size() - 1;
            }
        }
    }
    if (best) {
        result.grid[*best].chosen = true;
        configure(model.alerter, result.grid[*best].trigger, result.grid[*best].sustain,
                  model.normal_median_score, config);
    }
    return result;
}

std::vector<double> learned_scores(const LearnedModel& model, const ScenarioFeatures& features,
                                   const features::FeatureSchema& schema) {
    const Projection projection{schema};
    std::vector<double> out;
    out.reserve(features.vectors.size());
    for (const auto& v : features.vectors) {
        out.push_back(model.score(model.standardizer.transform(projection.raw(v))));
    }
    return out;
}

std::vector<Detection> run_learned(const LearnedModel& model, const ScenarioFeatures& features,
                                   const features::FeatureSchema& schema) {
    const Projection projection{schema};
    const auto& refs = learned_features();
    ScoreAlerter alerter{model.alerter};
    std::vector<Detection> out;
    for (const auto& v : features.vectors) {
        const auto raw = projection.raw(v);
        const auto z = model.standardizer.transform(raw);
        const double value = model.score(z);
        // Evidence: the score, then the three features that contribute most (k-NN: largest
        // |z|; perceptron: largest |w z|), with their raw values.
        const std::function<std::vector<Evidence>()> evidence = [&] {
            std::vector<std::pair<double, std::size_t>> parts;
            for (std::size_t d = 0; d < z.size(); ++d) {
                const double c = model.perceptron ? model.perceptron->weights()[d] * z[d] : z[d];
                parts.emplace_back(-std::abs(c), d);
            }
            std::stable_sort(parts.begin(), parts.end(),
                             [](const auto& a, const auto& b) { return a.first < b.first; });
            std::vector<Evidence> ev;
            ev.push_back(Evidence{model.alerter.detector + ".score", value});
            for (std::size_t i = 0; i < parts.size() && i < 3; ++i) {
                ev.push_back(
                    Evidence{features::feature_name(refs[parts[i].second]), raw[parts[i].second]});
            }
            return ev;
        };
        if (auto d = alerter.step(v.node, v.as_of, value, evidence)) {
            out.push_back(std::move(*d));
        }
    }
    return out;
}

}  // namespace faulttwin::detection
