#include "faulttwin/features/feature_extractor.hpp"

#include <utility>

namespace faulttwin::features {

std::string_view to_string(ObserveResult result) noexcept {
    switch (result) {
        case ObserveResult::kAccepted:
            return "accepted";
        case ObserveResult::kNotApplicable:
            return "not_applicable";
        case ObserveResult::kNotInSchema:
            return "not_in_schema";
        case ObserveResult::kOutOfOrder:
            return "out_of_order";
        case ObserveResult::kNonFinite:
            return "non_finite";
        case ObserveResult::kNodeLimit:
            return "node_limit";
    }
    return "unknown";
}

FeatureExtractor::FeatureExtractor(FeatureSchema schema, std::size_t max_nodes)
    : schema_(std::move(schema)), max_nodes_(max_nodes) {
    for (const MetricId metric : schema_.metrics()) {
        metric_used_.at(static_cast<std::size_t>(metric)) = true;
    }
}

ObserveResult FeatureExtractor::observe(const Observation& observation) {
    if (observation.quality == Quality::kNotApplicable) {
        ++counters_.not_applicable;
        return ObserveResult::kNotApplicable;
    }
    const auto metric_index = static_cast<std::size_t>(observation.metric);
    if (metric_index >= kMetricCount || !metric_used_.at(metric_index)) {
        ++counters_.not_in_schema;
        return ObserveResult::kNotInSchema;
    }

    auto it = nodes_.find(observation.node);
    if (it == nodes_.end()) {
        if (nodes_.size() >= max_nodes_) {
            ++counters_.node_limit;
            return ObserveResult::kNodeLimit;
        }
        it = nodes_.emplace(observation.node, NodeSeries{}).first;
    }
    Series& series = it->second.at(metric_index);
    if (!series.window) {
        series.window.emplace(schema_.window());
    }

    const bool ok = observation.quality == Quality::kOk;
    switch (series.window->push(observation.at, observation.value, ok)) {
        case PushResult::kOutOfOrder:
            ++counters_.out_of_order;
            return ObserveResult::kOutOfOrder;
        case PushResult::kNonFinite:
            ++counters_.non_finite;
            return ObserveResult::kNonFinite;
        case PushResult::kAccepted:
            break;
    }
    if (!series.first_seen_at) {
        series.first_seen_at = observation.at;
    }
    if (ok) {
        series.last_ok_at = observation.at;
    }
    ++counters_.accepted;
    return ObserveResult::kAccepted;
}

namespace {

std::optional<double> pick(const WindowStats& s, FeatureKind kind) {
    switch (kind) {
        case FeatureKind::kLast:
            return s.last;
        case FeatureKind::kMean:
            return s.mean;
        case FeatureKind::kStddev:
            return s.stddev;
        case FeatureKind::kMin:
            return s.min;
        case FeatureKind::kMax:
            return s.max;
        case FeatureKind::kRatePerSecond:
            return s.rate_per_second;
        case FeatureKind::kSlopePerSecond:
            return s.slope_per_second;
        case FeatureKind::kMissingRatio:
            return s.missing_ratio;
        case FeatureKind::kAgeSeconds:
            return std::nullopt;  // not a window statistic; handled by the caller
    }
    return std::nullopt;
}

}  // namespace

FeatureVector FeatureExtractor::extract(std::string_view node, Timestamp as_of) {
    FeatureVector out;
    out.schema_version = schema_.version();
    out.schema_fingerprint = schema_.fingerprint();
    out.node = std::string{node};
    out.as_of = as_of;
    out.values.assign(schema_.size(), std::nullopt);

    const auto it = nodes_.find(node);
    if (it == nodes_.end()) {
        return out;
    }

    // One stats() call per metric, shared by every feature of that metric.
    std::array<std::optional<WindowStats>, kMetricCount> stats{};
    std::array<bool, kMetricCount> behind{};
    for (const MetricId metric : schema_.metrics()) {
        const auto m = static_cast<std::size_t>(metric);
        Series& series = it->second.at(m);
        if (!series.window) {
            continue;
        }
        stats.at(m) = series.window->stats(as_of);
        if (!stats.at(m)) {
            behind.at(m) = true;
            ++counters_.clock_behind_data;
        }
    }

    const auto& refs = schema_.features();
    for (std::size_t i = 0; i < refs.size(); ++i) {
        const auto m = static_cast<std::size_t>(refs[i].metric);
        if (behind.at(m)) {
            continue;
        }
        if (refs[i].kind == FeatureKind::kAgeSeconds) {
            const Series& series = it->second.at(m);
            const auto since = series.last_ok_at ? series.last_ok_at : series.first_seen_at;
            if (since && *since <= as_of) {
                out.values[i] = std::chrono::duration<double>(as_of - *since).count();
            }
            continue;
        }
        if (stats.at(m)) {
            out.values[i] = pick(*stats.at(m), refs[i].kind);
        }
    }
    return out;
}

std::vector<std::string> FeatureExtractor::nodes() const {
    std::vector<std::string> out;
    out.reserve(nodes_.size());
    for (const auto& [name, series] : nodes_) {
        out.push_back(name);
    }
    return out;
}

}  // namespace faulttwin::features
