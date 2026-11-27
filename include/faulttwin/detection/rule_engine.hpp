#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "faulttwin/detection/rule.hpp"
#include "faulttwin/features/feature_schema.hpp"

namespace faulttwin::detection {

using features::Timestamp;

enum class DetectionKind : std::uint8_t { kFired, kEscalated, kResolved };
[[nodiscard]] std::string_view to_string(DetectionKind kind) noexcept;

/// One feature value that a detection is based on (evidence for reports and, later, for the
/// explanation service). nullopt values are reported as such.
struct Evidence {
    std::string feature;
    std::optional<double> value;
};

/// A state change of one alert. Typed and self-describing: rule set version, rule id, node,
/// the event time it happened at and when the violation began.
struct Detection {
    DetectionKind kind{DetectionKind::kFired};
    std::string rule_set_version;
    std::string rule_id;
    std::string node;
    Severity severity{Severity::kWarning};
    Timestamp at;               ///< evaluation time of the state change
    Timestamp violation_since;  ///< first tick of the violation that led to this alert
    std::vector<Evidence> evidence;
};

struct EngineCounters {
    std::uint64_t evaluations{0};  ///< (rule, node) evaluations
    std::uint64_t unknown{0};      ///< evaluations where the condition was unknown
    std::uint64_t fired{0};
    std::uint64_t escalated{0};
    std::uint64_t resolved{0};
    std::uint64_t suppressed_cooldown{0};  ///< ticks where a sustained violation waited
    std::uint64_t rejected_vectors{0};     ///< schema mismatch, time going backwards, node limit
};

enum class EvaluateError : std::uint8_t {
    kSchemaMismatch,     ///< vector built with another schema version / fingerprint
    kTimeWentBackwards,  ///< as_of earlier than the previous evaluation of this node
    kNodeLimit,          ///< a new node beyond max_nodes (bounded memory)
};
[[nodiscard]] std::string_view to_string(EvaluateError error) noexcept;

/// Evaluates a validated RuleSet against feature vectors. Deterministic: the same sequence of
/// vectors yields the same detections. State is kept per (rule, node), bounded by max_nodes.
/// Not thread-safe (one engine per evaluation thread).
class RuleEngine {
  public:
    /// Throws std::invalid_argument if validate_rule_set reports any issue: a broken rule set
    /// is a configuration error, caught at startup rather than per tick.
    RuleEngine(RuleSet rules, features::FeatureSchema schema, std::size_t max_nodes = 64);

    /// Evaluates every rule for vector.node at vector.as_of and returns the state changes in
    /// rule order. Ticks for one node must have non-decreasing as_of.
    [[nodiscard]] std::variant<std::vector<Detection>, EvaluateError> evaluate(
        const features::FeatureVector& vector);

    /// Alerts currently firing, as (rule id, node) in deterministic order.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> firing() const;
    [[nodiscard]] const RuleSet& rules() const noexcept { return rules_; }
    [[nodiscard]] const EngineCounters& counters() const noexcept { return counters_; }

  private:
    enum class Phase : std::uint8_t { kInactive, kPending, kFiring, kRecovering };
    struct AlertState {
        Phase phase{Phase::kInactive};
        Timestamp violation_since;
        Timestamp recovering_since;
        std::optional<Timestamp> cooldown_until;
        Severity severity{Severity::kWarning};
    };
    struct NodeState {
        std::optional<Timestamp> last_as_of;
        std::vector<AlertState> alerts;  ///< one per rule, in rule order
    };

    void step_quiet(std::size_t rule_index, AlertState& state,
                    const features::FeatureVector& vector, std::vector<Detection>& out);
    void step_active(std::size_t rule_index, AlertState& state,
                     const features::FeatureVector& vector, std::vector<Detection>& out);
    [[nodiscard]] bool critical_now(std::size_t rule_index,
                                    const features::FeatureVector& vector) const;
    [[nodiscard]] Detection make_detection(std::size_t rule_index, const AlertState& state,
                                           const features::FeatureVector& vector,
                                           DetectionKind kind) const;

    RuleSet rules_;
    features::FeatureSchema schema_;
    std::vector<std::vector<std::size_t>> feature_index_;  ///< [rule][predicate] -> column
    std::size_t max_nodes_;
    std::map<std::string, NodeState, std::less<>> nodes_;
    EngineCounters counters_;
};

}  // namespace faulttwin::detection
