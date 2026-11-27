#include "faulttwin/detection/rule_engine.hpp"

#include <stdexcept>

namespace faulttwin::detection {

std::string_view to_string(DetectionKind kind) noexcept {
    switch (kind) {
        case DetectionKind::kFired:
            return "fired";
        case DetectionKind::kEscalated:
            return "escalated";
        case DetectionKind::kResolved:
            return "resolved";
    }
    return "unknown";
}

std::string_view to_string(EvaluateError error) noexcept {
    switch (error) {
        case EvaluateError::kSchemaMismatch:
            return "schema_mismatch";
        case EvaluateError::kTimeWentBackwards:
            return "time_went_backwards";
        case EvaluateError::kNodeLimit:
            return "node_limit";
    }
    return "unknown";
}

namespace {

enum class Tri : std::uint8_t { kFalse, kTrue, kUnknown };

enum class Level : std::uint8_t { kTrigger, kClear, kCritical };

/// nullopt when the predicate has no threshold at this level (critical not configured).
std::optional<Tri> test(const Predicate& p, std::optional<double> value, Level level) {
    double threshold = p.trigger;
    if (level == Level::kClear) {
        threshold = p.clear;
    } else if (level == Level::kCritical) {
        if (!p.critical) {
            return std::nullopt;
        }
        threshold = *p.critical;
    }
    if (!value) {
        return Tri::kUnknown;
    }
    // Trigger and critical are strict ("beyond"); at the clear level the question is "still
    // violated?", i.e. not yet back on the healthy side of `clear`.
    const bool beyond =
        p.comparison == Comparison::kAbove ? *value > threshold : *value < threshold;
    return beyond ? Tri::kTrue : Tri::kFalse;
}

Tri combine(Combiner combiner, const std::vector<Tri>& parts) {
    if (parts.empty()) {
        return Tri::kFalse;
    }
    bool any_unknown = false;
    for (const Tri t : parts) {
        if (combiner == Combiner::kAll && t == Tri::kFalse) {
            return Tri::kFalse;
        }
        if (combiner == Combiner::kAny && t == Tri::kTrue) {
            return Tri::kTrue;
        }
        any_unknown = any_unknown || t == Tri::kUnknown;
    }
    if (any_unknown) {
        return Tri::kUnknown;
    }
    return combiner == Combiner::kAll ? Tri::kTrue : Tri::kFalse;
}

}  // namespace

RuleEngine::RuleEngine(RuleSet rules, features::FeatureSchema schema, std::size_t max_nodes)
    : rules_(std::move(rules)), schema_(std::move(schema)), max_nodes_(max_nodes) {
    const auto issues = validate_rule_set(rules_, schema_);
    if (!issues.empty()) {
        throw std::invalid_argument("invalid rule set: " + issues.front().rule_id + ": " +
                                    issues.front().message);
    }
    feature_index_.reserve(rules_.rules.size());
    for (const auto& rule : rules_.rules) {
        std::vector<std::size_t> columns;
        columns.reserve(rule.predicates.size());
        for (const auto& p : rule.predicates) {
            columns.push_back(schema_.index_of(p.feature).value_or(0));
        }
        feature_index_.push_back(std::move(columns));
    }
}

namespace {

/// The rule's condition at one threshold level; nullopt when no predicate has that level.
std::optional<Tri> evaluate_level(const Rule& rule, const std::vector<std::size_t>& columns,
                                  const std::vector<std::optional<double>>& values, Level level) {
    std::vector<Tri> parts;
    parts.reserve(rule.predicates.size());
    for (std::size_t p = 0; p < rule.predicates.size(); ++p) {
        if (const auto result = test(rule.predicates[p], values[columns[p]], level)) {
            parts.push_back(*result);
        }
    }
    if (parts.empty()) {
        return std::nullopt;
    }
    return combine(rule.combiner, parts);
}

}  // namespace

std::variant<std::vector<Detection>, EvaluateError> RuleEngine::evaluate(
    const features::FeatureVector& vector) {
    if (vector.schema_version != schema_.version() ||
        vector.schema_fingerprint != schema_.fingerprint() ||
        vector.values.size() != schema_.size()) {
        ++counters_.rejected_vectors;
        return EvaluateError::kSchemaMismatch;
    }
    auto it = nodes_.find(vector.node);
    if (it == nodes_.end()) {
        if (nodes_.size() >= max_nodes_) {
            ++counters_.rejected_vectors;
            return EvaluateError::kNodeLimit;
        }
        NodeState fresh;
        fresh.alerts.resize(rules_.rules.size());
        it = nodes_.emplace(vector.node, std::move(fresh)).first;
    }
    NodeState& node = it->second;
    if (node.last_as_of && vector.as_of < *node.last_as_of) {
        ++counters_.rejected_vectors;
        return EvaluateError::kTimeWentBackwards;
    }
    node.last_as_of = vector.as_of;

    std::vector<Detection> out;
    for (std::size_t r = 0; r < rules_.rules.size(); ++r) {
        ++counters_.evaluations;
        AlertState& state = node.alerts[r];
        if (state.phase == Phase::kInactive || state.phase == Phase::kPending) {
            step_quiet(r, state, vector, out);
        } else {
            step_active(r, state, vector, out);
        }
    }
    return out;
}

Detection RuleEngine::make_detection(std::size_t rule_index, const AlertState& state,
                                     const features::FeatureVector& vector,
                                     DetectionKind kind) const {
    const Rule& rule = rules_.rules[rule_index];
    Detection d;
    d.kind = kind;
    d.rule_set_version = rules_.version;
    d.rule_id = rule.id;
    d.node = vector.node;
    d.severity = state.severity;
    d.at = vector.as_of;
    d.violation_since = state.violation_since;
    for (std::size_t p = 0; p < rule.predicates.size(); ++p) {
        d.evidence.push_back(Evidence{features::feature_name(rule.predicates[p].feature),
                                      vector.values[feature_index_[rule_index][p]]});
    }
    return d;
}

bool RuleEngine::critical_now(std::size_t rule_index, const features::FeatureVector& vector) const {
    return evaluate_level(rules_.rules[rule_index], feature_index_[rule_index], vector.values,
                          Level::kCritical)
               .value_or(Tri::kFalse) == Tri::kTrue;
}

void RuleEngine::step_quiet(std::size_t rule_index, AlertState& state,
                            const features::FeatureVector& vector, std::vector<Detection>& out) {
    const Rule& rule = rules_.rules[rule_index];
    const Timestamp now = vector.as_of;
    const Tri violated =
        evaluate_level(rule, feature_index_[rule_index], vector.values, Level::kTrigger)
            .value_or(Tri::kFalse);
    if (violated == Tri::kUnknown) {
        ++counters_.unknown;
    }
    if (violated != Tri::kTrue) {
        // Unknown does not continue a pending violation: the sustain window must be covered
        // by data that shows the violation.
        state.phase = Phase::kInactive;
        return;
    }
    if (state.phase == Phase::kInactive) {
        state.phase = Phase::kPending;
        state.violation_since = now;
    }
    if (now - state.violation_since < rule.sustain) {
        return;
    }
    if (state.cooldown_until && now < *state.cooldown_until) {
        ++counters_.suppressed_cooldown;
        return;
    }
    state.phase = Phase::kFiring;
    state.severity = critical_now(rule_index, vector) ? Severity::kCritical : rule.severity;
    state.cooldown_until.reset();
    ++counters_.fired;
    out.push_back(make_detection(rule_index, state, vector, DetectionKind::kFired));
}

void RuleEngine::step_active(std::size_t rule_index, AlertState& state,
                             const features::FeatureVector& vector, std::vector<Detection>& out) {
    const Rule& rule = rules_.rules[rule_index];
    const Timestamp now = vector.as_of;
    const Tri still = evaluate_level(rule, feature_index_[rule_index], vector.values, Level::kClear)
                          .value_or(Tri::kFalse);
    if (still == Tri::kUnknown) {
        ++counters_.unknown;
        return;  // hold: missing data neither confirms nor clears an alert
    }
    if (still == Tri::kTrue) {
        state.phase = Phase::kFiring;
        if (state.severity != Severity::kCritical && critical_now(rule_index, vector)) {
            state.severity = Severity::kCritical;
            ++counters_.escalated;
            out.push_back(make_detection(rule_index, state, vector, DetectionKind::kEscalated));
        }
        return;
    }
    if (state.phase == Phase::kFiring) {
        state.phase = Phase::kRecovering;
        state.recovering_since = now;
    }
    if (now - state.recovering_since >= rule.clear_for) {
        out.push_back(make_detection(rule_index, state, vector, DetectionKind::kResolved));
        ++counters_.resolved;
        state.phase = Phase::kInactive;
        state.severity = rule.severity;
        state.cooldown_until = now + rule.cooldown;
    }
}

std::vector<std::pair<std::string, std::string>> RuleEngine::firing() const {
    std::vector<std::pair<std::string, std::string>> out;
    for (std::size_t r = 0; r < rules_.rules.size(); ++r) {
        for (const auto& [name, state] : nodes_) {
            const Phase phase = state.alerts[r].phase;
            if (phase == Phase::kFiring || phase == Phase::kRecovering) {
                out.emplace_back(rules_.rules[r].id, name);
            }
        }
    }
    return out;
}

}  // namespace faulttwin::detection
