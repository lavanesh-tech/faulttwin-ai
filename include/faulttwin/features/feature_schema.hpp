#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "faulttwin/features/observation.hpp"
#include "faulttwin/features/rolling_window.hpp"

namespace faulttwin::features {

/// What is computed from one metric's window.
enum class FeatureKind : std::uint8_t {
    kLast,    ///< newest ok value
    kMean,    ///< mean of ok values (Welford)
    kStddev,  ///< sample standard deviation of ok values (Welford, n - 1)
    kMin,
    kMax,
    kRatePerSecond,   ///< (last - first) / elapsed, over ok values
    kSlopePerSecond,  ///< least-squares trend of ok values over event time
    kMissingRatio,    ///< not-ok entries / entries in the window
    kAgeSeconds,      ///< as_of - event time of the newest ok value ever seen (staleness)
};

inline constexpr std::size_t kFeatureKindCount = 9;

[[nodiscard]] std::string_view to_string(FeatureKind kind) noexcept;
[[nodiscard]] std::optional<FeatureKind> feature_kind_from_string(std::string_view name) noexcept;

struct FeatureRef {
    MetricId metric{MetricId::kAvailability};
    FeatureKind kind{FeatureKind::kLast};
    friend bool operator==(const FeatureRef&, const FeatureRef&) = default;
};

/// "<metric>.<kind>", e.g. "replication_lag_seconds.slope_per_second".
[[nodiscard]] std::string feature_name(FeatureRef ref);
[[nodiscard]] std::optional<FeatureRef> parse_feature_name(std::string_view name);

/// An ordered, versioned list of features plus the window they are computed over. The order
/// is the column order of every FeatureVector (and of the Phase 7 model input). Changing the
/// list, the order or the window parameters requires a new version string; the fingerprint
/// makes an accidental change visible (a golden test pins it).
class FeatureSchema {
  public:
    FeatureSchema(std::string version, WindowConfig window, std::vector<FeatureRef> features);

    [[nodiscard]] const std::string& version() const noexcept { return version_; }
    [[nodiscard]] const WindowConfig& window() const noexcept { return window_; }
    [[nodiscard]] const std::vector<FeatureRef>& features() const noexcept { return features_; }
    [[nodiscard]] std::size_t size() const noexcept { return features_.size(); }
    [[nodiscard]] std::optional<std::size_t> index_of(FeatureRef ref) const noexcept;
    /// Metrics that at least one feature reads, in first-use order.
    [[nodiscard]] const std::vector<MetricId>& metrics() const noexcept { return metrics_; }
    /// FNV-1a 64 over the version, the window parameters and the feature names in order.
    [[nodiscard]] std::uint64_t fingerprint() const noexcept { return fingerprint_; }

  private:
    std::string version_;
    WindowConfig window_;
    std::vector<FeatureRef> features_;
    std::vector<MetricId> metrics_;
    std::uint64_t fingerprint_{0};
};

/// The Phase 4 schema, version "faulttwin.features.v1": 60 s window, capacity 256, every
/// feature kind for every metric (10 × 9 = 90 columns).
[[nodiscard]] const FeatureSchema& default_feature_schema();

/// Features of one node as of one event time, aligned with a schema. nullopt = could not be
/// computed (no data, too little data, not applicable), never a substitute 0.
struct FeatureVector {
    std::string schema_version;
    std::uint64_t schema_fingerprint{0};
    std::string node;
    Timestamp as_of;
    std::vector<std::optional<double>> values;

    [[nodiscard]] std::optional<double> get(const FeatureSchema& schema, FeatureRef ref) const;
};

}  // namespace faulttwin::features
