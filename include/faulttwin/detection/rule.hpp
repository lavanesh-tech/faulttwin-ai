#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "faulttwin/features/feature_schema.hpp"

namespace faulttwin::detection {

using features::FeatureKind;
using features::FeatureRef;
using features::MetricId;

enum class Severity : std::uint8_t { kWarning, kCritical };
[[nodiscard]] std::string_view to_string(Severity severity) noexcept;

enum class Comparison : std::uint8_t {
    kAbove,  ///< violated when value > trigger; recovered when value <= clear (clear <= trigger)
    kBelow,  ///< violated when value < trigger; recovered when value >= clear (clear >= trigger)
};

/// One threshold test on one feature, with hysteresis: a firing alert only recovers once the
/// value crosses `clear`, which sits on the healthy side of `trigger`, so a value hovering at
/// the threshold does not flap. `critical`, if set, escalates a firing alert.
struct Predicate {
    FeatureRef feature;
    Comparison comparison{Comparison::kAbove};
    double trigger{0.0};
    double clear{0.0};
    std::optional<double> critical;
};

/// How predicates combine. Three-valued: a predicate on a feature that is nullopt is
/// *unknown*. kAll is violated when every predicate is violated (unknown blocks it); kAny when
/// at least one is violated. Missing data is never read as healthy *or* as violating; stale
/// telemetry is detected by its own rule on the age feature.
enum class Combiner : std::uint8_t { kAll, kAny };

/// A detection rule. State machine per (rule, node), driven by evaluation ticks:
///   inactive --violated--> pending --violated for `sustain`--> firing (emits kFired)
///   firing --recovered for `clear_for`--> inactive (emits kResolved), then `cooldown`
///   during which a new violation cannot fire (counted as suppressed).
/// A firing alert escalates to kCritical (emits kEscalated) when every predicate with a
/// `critical` level is beyond it (kAll) / any is (kAny). Severity does not de-escalate until
/// the alert resolves.
struct Rule {
    std::string id;           ///< stable, snake_case
    std::string description;  ///< one line, for reports
    Combiner combiner{Combiner::kAll};
    std::vector<Predicate> predicates;
    std::chrono::milliseconds sustain{0};
    std::chrono::milliseconds clear_for{0};
    std::chrono::milliseconds cooldown{0};
    Severity severity{Severity::kWarning};
};

/// A named, versioned set of rules. The version is recorded with every detection so that a
/// replay or an evaluation can say which thresholds produced it.
struct RuleSet {
    std::string version;
    std::vector<Rule> rules;
};

struct RuleIssue {
    std::string rule_id;
    std::string message;
};

/// Static checks against a schema: non-empty unique ids, at least one predicate, every
/// feature present in the schema, finite thresholds, hysteresis on the healthy side,
/// critical beyond trigger, non-negative durations. An empty result means the set is usable.
[[nodiscard]] std::vector<RuleIssue> validate_rule_set(const RuleSet& rules,
                                                       const features::FeatureSchema& schema);

/// Tunable parameters of the default rules (docs/phase-reports/phase-04.md §12).
struct DefaultRuleParams {
    // Triggers and sustains: the values `faulttwin-detect evaluate` chose on validation seeds
    // 1-40 (results/phase-04/detection/linux-x86_64-container/summary.json). Hysteresis,
    // clear_for and cooldown are fixed design values, not tuned.
    double lag_trigger_seconds{5.0};
    std::chrono::milliseconds lag_sustain{std::chrono::seconds{10}};
    double lag_growth_trigger_per_second{0.1};
    std::chrono::milliseconds lag_growth_sustain{std::chrono::seconds{15}};
    double cpu_trigger_percent{80.0};
    std::chrono::milliseconds cpu_sustain{std::chrono::seconds{15}};
    double latency_trigger_ms{30.0};
    std::chrono::milliseconds latency_sustain{std::chrono::seconds{0}};
    double stale_after_seconds{1.5};
    std::chrono::milliseconds stale_sustain{std::chrono::seconds{2}};
    /// Hysteresis: clear level = trigger × clear_ratio (for kAbove rules).
    double clear_ratio{0.8};
    std::chrono::milliseconds clear_for{std::chrono::seconds{5}};
    std::chrono::milliseconds cooldown{std::chrono::seconds{30}};
};

/// The Phase 4 rule baseline ("faulttwin.rules.v1"): replication lag high, replication lag
/// growing (multi-signal: slope and level), CPU saturation, statement latency with connection
/// pressure (multi-signal), and stale telemetry (no ok availability sample for a while).
[[nodiscard]] RuleSet default_rule_set(const DefaultRuleParams& params = {});

}  // namespace faulttwin::detection
