#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "faulttwin/features/feature_schema.hpp"
#include "faulttwin/features/observation.hpp"
#include "faulttwin/features/rolling_window.hpp"

namespace faulttwin::features {

enum class ObserveResult : std::uint8_t {
    kAccepted,
    kNotApplicable,  ///< quality kNotApplicable: ignored, not missing data
    kNotInSchema,    ///< the schema reads no feature of this metric
    kOutOfOrder,     ///< event time not after the series' newest entry (duplicates included)
    kNonFinite,      ///< kOk with NaN or infinity
    kNodeLimit,      ///< a new node beyond max_nodes (bounded memory)
};

[[nodiscard]] std::string_view to_string(ObserveResult result) noexcept;

struct ExtractorCounters {
    std::uint64_t accepted{0};
    std::uint64_t not_applicable{0};
    std::uint64_t not_in_schema{0};
    std::uint64_t out_of_order{0};
    std::uint64_t non_finite{0};
    std::uint64_t node_limit{0};
    /// extract() calls for a series whose newest entry is after as_of (its features are nullopt)
    std::uint64_t clock_behind_data{0};
};

/// Turns observations into feature vectors with event-time semantics:
/// - each (node, metric) series has its own RollingWindow; observations must arrive in
///   increasing event time per series (allowed lateness 0, see ADR-0016);
/// - extract(node, as_of) uses the entries in (as_of - span, as_of];
/// - ok values feed the statistics; missing, stale and invalid values are counted in the
///   missing ratio and never become numbers; not-applicable values are ignored;
/// - the result depends only on the observation sequence and as_of, never on wall-clock time
///   or arrival timing, so a replay produces identical vectors.
/// Not thread-safe: one extractor per evaluation thread (ADR-0016).
class FeatureExtractor {
  public:
    explicit FeatureExtractor(FeatureSchema schema, std::size_t max_nodes = 64);

    ObserveResult observe(const Observation& observation);
    /// Features of `node` as of `as_of`. An unknown node yields a vector of nullopt values.
    [[nodiscard]] FeatureVector extract(std::string_view node, Timestamp as_of);

    /// Known nodes in lexicographic order (deterministic iteration).
    [[nodiscard]] std::vector<std::string> nodes() const;
    [[nodiscard]] const FeatureSchema& schema() const noexcept { return schema_; }
    [[nodiscard]] const ExtractorCounters& counters() const noexcept { return counters_; }

  private:
    struct Series {
        std::optional<RollingWindow> window;
        std::optional<Timestamp> first_seen_at;
        std::optional<Timestamp> last_ok_at;
    };
    using NodeSeries = std::array<Series, kMetricCount>;

    FeatureSchema schema_;
    std::size_t max_nodes_;
    std::array<bool, kMetricCount> metric_used_{};
    std::map<std::string, NodeSeries, std::less<>> nodes_;
    ExtractorCounters counters_;
};

}  // namespace faulttwin::features
