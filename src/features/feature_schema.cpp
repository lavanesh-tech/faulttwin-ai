#include "faulttwin/features/feature_schema.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace faulttwin::features {

namespace {

constexpr std::array<std::string_view, kMetricCount> kMetricNames{
    "availability",
    "probe_latency_ms",
    "replication_lag_seconds",
    "replication_lag_bytes",
    "connection_utilization",
    "commits_per_second",
    "statement_latency_ms",
    "cpu_percent",
    "memory_percent",
    "disk_percent",
};

constexpr std::array<std::string_view, kFeatureKindCount> kKindNames{
    "last",          "mean",        "stddev", "min", "max", "rate_per_second", "slope_per_second",
    "missing_ratio", "age_seconds",
};

constexpr std::array<FeatureKind, kFeatureKindCount> kAllKinds{
    FeatureKind::kLast,
    FeatureKind::kMean,
    FeatureKind::kStddev,
    FeatureKind::kMin,
    FeatureKind::kMax,
    FeatureKind::kRatePerSecond,
    FeatureKind::kSlopePerSecond,
    FeatureKind::kMissingRatio,
    FeatureKind::kAgeSeconds,
};

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void fnv1a(std::uint64_t& hash, std::string_view bytes) {
    for (const char c : bytes) {
        hash ^= static_cast<unsigned char>(c);
        hash *= kFnvPrime;
    }
    hash ^= 0xFFU;  // field separator, so ("ab","c") and ("a","bc") differ
    hash *= kFnvPrime;
}

}  // namespace

std::string_view to_string(MetricId metric) noexcept {
    const auto i = static_cast<std::size_t>(metric);
    return i < kMetricNames.size() ? kMetricNames.at(i) : std::string_view{"unknown"};
}

std::optional<MetricId> metric_from_string(std::string_view name) noexcept {
    for (std::size_t i = 0; i < kMetricNames.size(); ++i) {
        if (kMetricNames.at(i) == name) {
            return kAllMetrics.at(i);
        }
    }
    return std::nullopt;
}

std::string_view to_string(FeatureKind kind) noexcept {
    const auto i = static_cast<std::size_t>(kind);
    return i < kKindNames.size() ? kKindNames.at(i) : std::string_view{"unknown"};
}

std::optional<FeatureKind> feature_kind_from_string(std::string_view name) noexcept {
    for (std::size_t i = 0; i < kKindNames.size(); ++i) {
        if (kKindNames.at(i) == name) {
            return kAllKinds.at(i);
        }
    }
    return std::nullopt;
}

std::string feature_name(FeatureRef ref) {
    std::string name{to_string(ref.metric)};
    name += '.';
    name += to_string(ref.kind);
    return name;
}

std::optional<FeatureRef> parse_feature_name(std::string_view name) {
    const auto dot = name.find('.');
    if (dot == std::string_view::npos) {
        return std::nullopt;
    }
    const auto metric = metric_from_string(name.substr(0, dot));
    const auto kind = feature_kind_from_string(name.substr(dot + 1));
    if (!metric || !kind) {
        return std::nullopt;
    }
    return FeatureRef{*metric, *kind};
}

FeatureSchema::FeatureSchema(std::string version, WindowConfig window,
                             std::vector<FeatureRef> features)
    : version_(std::move(version)), window_(window), features_(std::move(features)) {
    if (version_.empty()) {
        throw std::invalid_argument("FeatureSchema version must not be empty");
    }
    if (features_.empty()) {
        throw std::invalid_argument("FeatureSchema needs at least one feature");
    }
    for (std::size_t i = 0; i < features_.size(); ++i) {
        const auto& f = features_[i];
        if (std::find(features_.begin(), features_.begin() + static_cast<std::ptrdiff_t>(i), f) !=
            features_.begin() + static_cast<std::ptrdiff_t>(i)) {
            throw std::invalid_argument("FeatureSchema lists " + feature_name(f) + " twice");
        }
        if (std::find(metrics_.begin(), metrics_.end(), f.metric) == metrics_.end()) {
            metrics_.push_back(f.metric);
        }
    }
    // Validates the window parameters with the same rules as the windows themselves.
    (void)RollingWindow{window_};

    fingerprint_ = kFnvOffset;
    fnv1a(fingerprint_, version_);
    fnv1a(fingerprint_, std::to_string(window_.span.count()));
    fnv1a(fingerprint_, std::to_string(window_.capacity));
    for (const auto& f : features_) {
        fnv1a(fingerprint_, feature_name(f));
    }
}

std::optional<std::size_t> FeatureSchema::index_of(FeatureRef ref) const noexcept {
    const auto it = std::find(features_.begin(), features_.end(), ref);
    if (it == features_.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(it - features_.begin());
}

const FeatureSchema& default_feature_schema() {
    static const FeatureSchema schema = [] {
        std::vector<FeatureRef> refs;
        refs.reserve(kMetricCount * kFeatureKindCount);
        for (const MetricId metric : kAllMetrics) {
            for (const FeatureKind kind : kAllKinds) {
                refs.push_back(FeatureRef{metric, kind});
            }
        }
        WindowConfig window;
        window.span = std::chrono::seconds{60};
        window.capacity = 256;
        return FeatureSchema{"faulttwin.features.v1", window, std::move(refs)};
    }();
    return schema;
}

std::optional<double> FeatureVector::get(const FeatureSchema& schema, FeatureRef ref) const {
    if (schema.version() != schema_version || schema.fingerprint() != schema_fingerprint) {
        return std::nullopt;
    }
    const auto index = schema.index_of(ref);
    if (!index || *index >= values.size()) {
        return std::nullopt;
    }
    return values[*index];
}

}  // namespace faulttwin::features
