#include "faulttwin/detection/evaluation.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <variant>

namespace faulttwin::detection {

ScenarioFeatures extract_scenario_features(const Scenario& scenario,
                                           const features::FeatureSchema& schema) {
    features::FeatureExtractor extractor{schema};
    ScenarioFeatures out;
    const auto& config = scenario.config;
    const auto tick = config.tick.count() > 0 ? config.tick : std::chrono::milliseconds{1000};
    const auto ticks =
        std::chrono::duration_cast<std::chrono::milliseconds>(config.duration).count() /
        tick.count();
    out.vectors.reserve(static_cast<std::size_t>(ticks) * scenario.nodes.size());

    std::size_t next = 0;
    const auto& obs = scenario.observations;
    for (std::int64_t t = 0; t < ticks; ++t) {
        const Timestamp now = config.start + tick * t;
        while (next < obs.size() && obs[next].at <= now) {
            (void)extractor.observe(obs[next]);
            ++next;
        }
        for (const auto& node : scenario.nodes) {
            out.vectors.push_back(extractor.extract(node, now));
        }
    }
    out.counters = extractor.counters();
    return out;
}

std::vector<Detection> run_rules(const ScenarioFeatures& features, const RuleSet& rules,
                                 const features::FeatureSchema& schema) {
    RuleEngine engine{rules, schema};
    std::vector<Detection> out;
    for (const auto& vector : features.vectors) {
        auto result = engine.evaluate(vector);
        if (auto* detections = std::get_if<std::vector<Detection>>(&result)) {
            for (auto& d : *detections) {
                out.push_back(std::move(d));
            }
        }
    }
    return out;
}

void EvaluationCounts::merge(const EvaluationCounts& other) {
    alerts += other.alerts;
    matched_alerts += other.matched_alerts;
    false_alerts += other.false_alerts;
    episodes += other.episodes;
    detected += other.detected;
    negative_hours += other.negative_hours;
    for (std::size_t k = 0; k < kFaultKindCount; ++k) {
        by_kind.at(k).episodes += other.by_kind.at(k).episodes;
        by_kind.at(k).detected += other.by_kind.at(k).detected;
        by_kind.at(k).latencies_seconds.insert(by_kind.at(k).latencies_seconds.end(),
                                               other.by_kind.at(k).latencies_seconds.begin(),
                                               other.by_kind.at(k).latencies_seconds.end());
    }
    for (const auto& [rule, counts] : other.by_rule) {
        auto& mine = by_rule[rule];
        mine.alerts += counts.alerts;
        mine.matched += counts.matched;
        mine.false_alerts += counts.false_alerts;
    }
}

namespace {

double seconds_of(std::pair<Timestamp, Timestamp> span) {
    return std::chrono::duration<double>(span.second - span.first).count();
}

double negative_hours_for(const Scenario& scenario, const MatchConfig& match) {
    const Timestamp begin = scenario.config.start;
    const Timestamp finish = begin + scenario.config.duration;
    double seconds = 0.0;
    for (const auto& node : scenario.nodes) {
        std::vector<std::pair<Timestamp, Timestamp>> spans;
        for (const auto& e : scenario.episodes) {
            if (e.node == node) {
                spans.emplace_back(std::max(e.onset, begin), std::min(e.end + match.grace, finish));
            }
        }
        std::sort(spans.begin(), spans.end());
        // Union of the (sorted) spans: merge overlaps, sum the merged lengths.
        double covered = 0.0;
        for (std::size_t i = 0; i < spans.size();) {
            auto merged = spans[i];
            std::size_t j = i + 1;
            while (j < spans.size() && spans[j].first <= merged.second) {
                merged.second = std::max(merged.second, spans[j].second);
                ++j;
            }
            covered += seconds_of(merged);
            i = j;
        }
        seconds += seconds_of({begin, finish}) - covered;
    }
    return seconds / 3600.0;
}

/// Index of the episode an alert on `node` at `at` matches, if any.
std::optional<std::size_t> matching_episode(const Scenario& scenario, const std::string& node,
                                            Timestamp at, const MatchConfig& match) {
    for (std::size_t i = 0; i < scenario.episodes.size(); ++i) {
        const auto& e = scenario.episodes[i];
        if (e.node == node && at >= e.onset && at <= e.end + match.grace) {
            return i;
        }
    }
    return std::nullopt;
}

}  // namespace

EvaluationCounts score(const Scenario& scenario, const std::vector<Detection>& detections,
                       const MatchConfig& match, const std::function<bool(FaultKind)>& target) {
    EvaluationCounts counts;
    const auto counted = [&](FaultKind kind) { return !target || target(kind); };

    // Event time of the first alert per episode; Timestamp::max() = not detected.
    std::vector<Timestamp> first_alert(scenario.episodes.size(), Timestamp::max());
    for (const auto& d : detections) {
        if (d.kind != DetectionKind::kFired) {
            continue;
        }
        const std::size_t hit =
            matching_episode(scenario, d.node, d.at, match).value_or(scenario.episodes.size());
        const bool matched = hit < scenario.episodes.size();
        if (matched && !counted(scenario.episodes[hit].kind)) {
            continue;  // inside an episode this evaluation does not look at
        }
        auto& rule = counts.by_rule[d.rule_id];
        ++counts.alerts;
        ++rule.alerts;
        if (matched) {
            ++counts.matched_alerts;
            ++rule.matched;
            first_alert[hit] = std::min(first_alert[hit], d.at);
        } else {
            ++counts.false_alerts;
            ++rule.false_alerts;
        }
    }

    for (std::size_t i = 0; i < scenario.episodes.size(); ++i) {
        const auto& e = scenario.episodes[i];
        if (!counted(e.kind)) {
            continue;
        }
        auto& kind = counts.by_kind.at(static_cast<std::size_t>(e.kind));
        ++counts.episodes;
        ++kind.episodes;
        if (first_alert[i] != Timestamp::max()) {
            ++counts.detected;
            ++kind.detected;
            kind.latencies_seconds.push_back(
                std::chrono::duration<double>(first_alert[i] - e.onset).count());
        }
    }
    counts.negative_hours = negative_hours_for(scenario, match);
    return counts;
}

std::optional<double> nearest_rank_percentile(std::vector<double> values, double percentile) {
    if (values.empty()) {
        return std::nullopt;
    }
    std::sort(values.begin(), values.end());
    const double rank = std::ceil(percentile / 100.0 * static_cast<double>(values.size()));
    const auto index = static_cast<std::size_t>(std::max(rank, 1.0)) - 1;
    return values[std::min(index, values.size() - 1)];
}

namespace {

DetectionMetrics compute(std::size_t matched, std::size_t alerts, std::size_t detected,
                         std::size_t episodes, std::optional<double> fah,
                         const std::vector<double>& latencies) {
    DetectionMetrics m;
    if (alerts > 0) {
        m.precision = static_cast<double>(matched) / static_cast<double>(alerts);
    }
    if (episodes > 0) {
        m.recall = static_cast<double>(detected) / static_cast<double>(episodes);
        m.missed_rate = static_cast<double>(episodes - detected) / static_cast<double>(episodes);
    }
    if (m.precision && m.recall && *m.precision + *m.recall > 0.0) {
        m.f1 = 2.0 * *m.precision * *m.recall / (*m.precision + *m.recall);
    }
    m.false_alerts_per_hour = fah;
    m.latency_median_seconds = nearest_rank_percentile(latencies, 50.0);
    m.latency_p95_seconds = nearest_rank_percentile(latencies, 95.0);
    m.latency_max_seconds = nearest_rank_percentile(latencies, 100.0);
    return m;
}

std::vector<double> all_latencies(const EvaluationCounts& counts) {
    std::vector<double> out;
    for (const auto& k : counts.by_kind) {
        out.insert(out.end(), k.latencies_seconds.begin(), k.latencies_seconds.end());
    }
    return out;
}

}  // namespace

DetectionMetrics metrics(const EvaluationCounts& counts) {
    std::optional<double> fah;
    if (counts.negative_hours > 0.0) {
        fah = static_cast<double>(counts.false_alerts) / counts.negative_hours;
    }
    return compute(counts.matched_alerts, counts.alerts, counts.detected, counts.episodes, fah,
                   all_latencies(counts));
}

DetectionMetrics kind_metrics(const KindCounts& counts) {
    DetectionMetrics m =
        compute(0, 0, counts.detected, counts.episodes, std::nullopt, counts.latencies_seconds);
    return m;
}

DefaultRuleParams naive_params(DefaultRuleParams params) {
    params.clear_ratio = 1.0;
    params.clear_for = std::chrono::milliseconds{0};
    params.cooldown = std::chrono::milliseconds{0};
    params.lag_sustain = params.lag_growth_sustain = params.cpu_sustain = params.latency_sustain =
        params.stale_sustain = std::chrono::milliseconds{0};
    return params;
}

namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;

struct RuleGrid {
    std::string rule_id;
    FaultKind target;
    std::vector<double> triggers;
    std::vector<milliseconds> sustains;
    std::function<void(DefaultRuleParams&, double, milliseconds)> apply;
};

std::vector<RuleGrid> grids() {
    const std::vector<milliseconds> sustains{seconds{0},  seconds{2},  seconds{5},
                                             seconds{10}, seconds{15}, seconds{20}};
    return {
        {"replication_lag_high",
         FaultKind::kReplicationLagBuildup,
         {2.0, 5.0, 10.0, 15.0, 20.0, 30.0},
         sustains,
         [](DefaultRuleParams& p, double t, milliseconds s) {
             p.lag_trigger_seconds = t;
             p.lag_sustain = s;
         }},
        {"replication_lag_growing",
         FaultKind::kReplicationLagBuildup,
         {0.1, 0.25, 0.5, 1.0, 2.0},
         sustains,
         [](DefaultRuleParams& p, double t, milliseconds s) {
             p.lag_growth_trigger_per_second = t;
             p.lag_growth_sustain = s;
         }},
        {"cpu_saturation",
         FaultKind::kCpuSaturation,
         {80.0, 85.0, 90.0, 95.0},
         sustains,
         [](DefaultRuleParams& p, double t, milliseconds s) {
             p.cpu_trigger_percent = t;
             p.cpu_sustain = s;
         }},
        {"statement_latency_with_connection_pressure",
         FaultKind::kOverload,
         {20.0, 30.0, 50.0, 80.0, 120.0},
         sustains,
         [](DefaultRuleParams& p, double t, milliseconds s) {
             p.latency_trigger_ms = t;
             p.latency_sustain = s;
         }},
        {"telemetry_stale",
         FaultKind::kNodeDown,
         {1.5, 2.5, 3.5, 5.0, 10.0},
         sustains,
         [](DefaultRuleParams& p, double t, milliseconds s) {
             p.stale_after_seconds = t;
             p.stale_sustain = s;
         }},
    };
}

RuleSet only(RuleSet set, const std::string& id) {
    std::erase_if(set.rules, [&](const Rule& r) { return r.id != id; });
    return set;
}

/// True when candidate `a` is strictly better than `b` under the tuning objective.
bool better(const GridRow& a, const GridRow& b) {
    const double fa = a.metrics.f1.value_or(-1.0);
    const double fb = b.metrics.f1.value_or(-1.0);
    if (fa != fb) {
        return fa > fb;
    }
    if (a.false_alerts != b.false_alerts) {
        return a.false_alerts < b.false_alerts;
    }
    const double la = a.metrics.latency_median_seconds.value_or(1e300);
    const double lb = b.metrics.latency_median_seconds.value_or(1e300);
    return la < lb;
}

}  // namespace

TuningResult tune_default_rules(const std::vector<Scenario>& validation,
                                const std::vector<ScenarioFeatures>& features,
                                const features::FeatureSchema& schema, BaselineKind kind,
                                const MatchConfig& match) {
    TuningResult result;
    if (kind == BaselineKind::kNaiveThreshold) {
        result.params = naive_params(result.params);
    }
    for (const auto& grid : grids()) {
        const auto target = [&](FaultKind k) { return k == grid.target; };
        std::vector<milliseconds> sustains = grid.sustains;
        if (kind == BaselineKind::kNaiveThreshold) {
            sustains = {milliseconds{0}};
        }
        std::optional<std::size_t> best;
        const std::size_t first_row = result.grid.size();
        for (const double trigger : grid.triggers) {
            for (const auto sustain : sustains) {
                DefaultRuleParams params = result.params;
                grid.apply(params, trigger, sustain);
                const RuleSet rules = only(default_rule_set(params), grid.rule_id);
                EvaluationCounts total;
                for (std::size_t s = 0; s < validation.size() && s < features.size(); ++s) {
                    total.merge(
                        score(validation[s], run_rules(features[s], rules, schema), match, target));
                }
                GridRow row;
                row.rule_id = grid.rule_id;
                row.trigger = trigger;
                row.sustain = sustain;
                row.matched = total.matched_alerts;
                row.false_alerts = total.false_alerts;
                row.episodes = total.episodes;
                row.detected = total.detected;
                row.metrics = metrics(total);
                result.grid.push_back(row);
                if (!best || better(row, result.grid[*best])) {
                    best = result.grid.size() - 1;
                }
            }
        }
        // A grid without episodes carries no information: keep the defaults.
        if (best && *best >= first_row && result.grid[*best].episodes > 0) {
            result.grid[*best].chosen = true;
            grid.apply(result.params, result.grid[*best].trigger, result.grid[*best].sustain);
        }
    }
    return result;
}

}  // namespace faulttwin::detection
