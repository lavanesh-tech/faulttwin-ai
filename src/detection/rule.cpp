#include "faulttwin/detection/rule.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace faulttwin::detection {

std::string_view to_string(Severity severity) noexcept {
    switch (severity) {
        case Severity::kWarning:
            return "warning";
        case Severity::kCritical:
            return "critical";
    }
    return "unknown";
}

namespace {

void check_predicate(const Rule& rule, const Predicate& p, const features::FeatureSchema& schema,
                     std::vector<RuleIssue>& issues) {
    const auto add = [&](std::string message) {
        issues.push_back(
            RuleIssue{rule.id, features::feature_name(p.feature) + ": " + std::move(message)});
    };
    if (!schema.index_of(p.feature)) {
        add("feature is not in schema " + schema.version());
    }
    if (!std::isfinite(p.trigger) || !std::isfinite(p.clear) ||
        (p.critical && !std::isfinite(*p.critical))) {
        add("thresholds must be finite");
        return;
    }
    const bool above = p.comparison == Comparison::kAbove;
    if (above ? p.clear > p.trigger : p.clear < p.trigger) {
        add("clear level must be on the healthy side of the trigger (hysteresis)");
    }
    if (p.critical && (above ? *p.critical < p.trigger : *p.critical > p.trigger)) {
        add("critical level must be beyond the trigger");
    }
}

}  // namespace

std::vector<RuleIssue> validate_rule_set(const RuleSet& rules,
                                         const features::FeatureSchema& schema) {
    std::vector<RuleIssue> issues;
    if (rules.version.empty()) {
        issues.push_back(RuleIssue{"", "rule set version must not be empty"});
    }
    std::set<std::string, std::less<>> seen;
    for (const auto& rule : rules.rules) {
        if (rule.id.empty()) {
            issues.push_back(RuleIssue{"", "rule id must not be empty"});
        } else if (!seen.insert(rule.id).second) {
            issues.push_back(RuleIssue{rule.id, "duplicate rule id"});
        }
        if (rule.predicates.empty()) {
            issues.push_back(RuleIssue{rule.id, "rule needs at least one predicate"});
        }
        if (rule.sustain.count() < 0 || rule.clear_for.count() < 0 || rule.cooldown.count() < 0) {
            issues.push_back(RuleIssue{rule.id, "durations must not be negative"});
        }
        for (const auto& p : rule.predicates) {
            check_predicate(rule, p, schema, issues);
        }
    }
    return issues;
}

RuleSet default_rule_set(const DefaultRuleParams& params) {
    using std::chrono::seconds;
    const auto above = [&](MetricId metric, FeatureKind kind, double trigger,
                           std::optional<double> critical = std::nullopt) {
        return Predicate{FeatureRef{metric, kind}, Comparison::kAbove, trigger,
                         trigger * params.clear_ratio, critical};
    };
    const auto make = [&](std::string id, std::string description, Combiner combiner,
                          std::vector<Predicate> predicates, std::chrono::milliseconds sustain,
                          Severity severity) {
        Rule rule;
        rule.id = std::move(id);
        rule.description = std::move(description);
        rule.combiner = combiner;
        rule.predicates = std::move(predicates);
        rule.sustain = sustain;
        rule.clear_for = params.clear_for;
        rule.cooldown = params.cooldown;
        rule.severity = severity;
        return rule;
    };

    RuleSet set;
    set.version = "faulttwin.rules.v1";
    set.rules.push_back(make("replication_lag_high",
                             "replica replay lag above the trigger for the sustain period",
                             Combiner::kAll,
                             {above(MetricId::kReplicationLagSeconds, FeatureKind::kLast,
                                    params.lag_trigger_seconds, params.lag_trigger_seconds * 3.0)},
                             params.lag_sustain, Severity::kWarning));
    set.rules.push_back(make(
        "replication_lag_growing",
        "replica replay lag trending upwards and already above 1 s (multi-signal)", Combiner::kAll,
        {above(MetricId::kReplicationLagSeconds, FeatureKind::kSlopePerSecond,
               params.lag_growth_trigger_per_second),
         Predicate{FeatureRef{MetricId::kReplicationLagSeconds, FeatureKind::kLast},
                   Comparison::kAbove, 1.0, 0.5, std::nullopt}},
        params.lag_growth_sustain, Severity::kWarning));
    set.rules.push_back(
        make("cpu_saturation", "CPU above the trigger for the sustain period", Combiner::kAll,
             {above(MetricId::kCpuPercent, FeatureKind::kLast, params.cpu_trigger_percent)},
             params.cpu_sustain, Severity::kWarning));
    set.rules.push_back(
        make("statement_latency_with_connection_pressure",
             "statement latency high while connection utilization is high (multi-signal)",
             Combiner::kAll,
             {above(MetricId::kStatementLatencyMs, FeatureKind::kLast, params.latency_trigger_ms,
                    params.latency_trigger_ms * 4.0),
              Predicate{FeatureRef{MetricId::kConnectionUtilization, FeatureKind::kLast},
                        Comparison::kAbove, 60.0, 50.0, std::nullopt}},
             params.latency_sustain, Severity::kWarning));
    set.rules.push_back(
        make("telemetry_stale", "no ok availability sample from the node for longer than allowed",
             Combiner::kAll,
             {Predicate{FeatureRef{MetricId::kAvailability, FeatureKind::kAgeSeconds},
                        Comparison::kAbove, params.stale_after_seconds, params.stale_after_seconds,
                        std::nullopt}},
             params.stale_sustain, Severity::kCritical));
    return set;
}

}  // namespace faulttwin::detection
