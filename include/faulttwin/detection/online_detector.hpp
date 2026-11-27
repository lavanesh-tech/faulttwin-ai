#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "faulttwin/collectors/pg_snapshot.hpp"
#include "faulttwin/collectors/rates.hpp"
#include "faulttwin/detection/rule.hpp"
#include "faulttwin/detection/rule_engine.hpp"
#include "faulttwin/features/feature_extractor.hpp"

namespace faulttwin::detection {

struct OnlineCounters {
    std::uint64_t cycles{0};
    std::uint64_t observations{0};
    std::uint64_t observations_refused{0};  ///< out of order, non-finite, node limit
    std::uint64_t vectors_refused{0};       ///< rejected by the rule engine
};

/// Features + rules for a live monitor: one call per collection cycle. Each cycle's
/// observations are fed to the extractor, then every node of the cycle is evaluated as of
/// its own snapshot time. Single-threaded, like the monitor loop that owns it.
class OnlineDetector {
  public:
    explicit OnlineDetector(RuleSet rules = default_rule_set(),
                            features::FeatureSchema schema = features::default_feature_schema());

    std::vector<Detection> on_cycle(std::span<const collectors::PgNodeSnapshot> nodes,
                                    std::span<const std::optional<collectors::PgRates>> rates);
    /// Generic form: feeds `observations`, then evaluates every known node as of `as_of`
    /// (which must not be earlier than any observation fed so far for that node).
    std::vector<Detection> on_observations(const std::vector<features::Observation>& observations,
                                           Timestamp as_of);

    [[nodiscard]] std::vector<std::pair<std::string, std::string>> firing() const {
        return engine_.firing();
    }
    [[nodiscard]] const OnlineCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] const RuleEngine& engine() const noexcept { return engine_; }

  private:
    void feed(const std::vector<features::Observation>& observations);
    void evaluate(const std::string& node, Timestamp as_of, std::vector<Detection>& out);

    features::FeatureExtractor extractor_;
    RuleEngine engine_;
    OnlineCounters counters_;
};

}  // namespace faulttwin::detection
