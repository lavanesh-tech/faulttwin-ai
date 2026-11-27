#include "detect_report.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <system_error>

#include "faulttwin/detection/report.hpp"
#include "faulttwin/features/feature_schema.hpp"

namespace faulttwin::detect {

namespace det = faulttwin::detection;
using det::json_number;
using det::json_quote;

namespace {

double seconds_since(det::Timestamp start, det::Timestamp t) {
    return std::chrono::duration<double>(t - start).count();
}

std::string num(std::optional<double> v) {
    return json_number(v);
}

/// Fixed-point text with `digits` decimals, for the Markdown tables (JSON keeps full precision).
std::string decimals(double v, int digits) {
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), v,
                                      std::chars_format::fixed, digits);
    return result.ec == std::errc{} ? std::string(buffer.data(), result.ptr) : "N/A";
}

std::string pct(std::optional<double> v) {
    return v ? decimals(*v * 100.0, 1) + " %" : std::string{"N/A"};
}

std::string fixed(std::optional<double> v, int digits, const char* unit = "") {
    if (!v) {
        return "N/A";
    }
    return decimals(*v, digits) + unit;
}

std::string params_json(const det::DefaultRuleParams& p) {
    const auto ms = [](std::chrono::milliseconds d) {
        return json_number(static_cast<double>(d.count()) / 1000.0);
    };
    std::string out = "{";
    out += "\"lag_trigger_seconds\":" + num(p.lag_trigger_seconds);
    out += ",\"lag_sustain_seconds\":" + ms(p.lag_sustain);
    out += ",\"lag_growth_trigger_per_second\":" + num(p.lag_growth_trigger_per_second);
    out += ",\"lag_growth_sustain_seconds\":" + ms(p.lag_growth_sustain);
    out += ",\"cpu_trigger_percent\":" + num(p.cpu_trigger_percent);
    out += ",\"cpu_sustain_seconds\":" + ms(p.cpu_sustain);
    out += ",\"latency_trigger_ms\":" + num(p.latency_trigger_ms);
    out += ",\"latency_sustain_seconds\":" + ms(p.latency_sustain);
    out += ",\"stale_after_seconds\":" + num(p.stale_after_seconds);
    out += ",\"stale_sustain_seconds\":" + ms(p.stale_sustain);
    out += ",\"clear_ratio\":" + num(p.clear_ratio);
    out += ",\"clear_for_seconds\":" + ms(p.clear_for);
    out += ",\"cooldown_seconds\":" + ms(p.cooldown);
    out += "}";
    return out;
}

std::string metrics_json(const det::DetectionMetrics& m) {
    std::string out = "{";
    out += "\"precision\":" + num(m.precision);
    out += ",\"recall\":" + num(m.recall);
    out += ",\"f1\":" + num(m.f1);
    out += ",\"false_alerts_per_hour\":" + num(m.false_alerts_per_hour);
    out += ",\"missed_rate\":" + num(m.missed_rate);
    out += ",\"latency_median_seconds\":" + num(m.latency_median_seconds);
    out += ",\"latency_p95_seconds\":" + num(m.latency_p95_seconds);
    out += ",\"latency_max_seconds\":" + num(m.latency_max_seconds);
    out += "}";
    return out;
}

std::string counts_json(const det::EvaluationCounts& c) {
    std::string out = "{";
    out += "\"alerts\":" + std::to_string(c.alerts);
    out += ",\"matched_alerts\":" + std::to_string(c.matched_alerts);
    out += ",\"false_alerts\":" + std::to_string(c.false_alerts);
    out += ",\"episodes\":" + std::to_string(c.episodes);
    out += ",\"detected\":" + std::to_string(c.detected);
    out += ",\"negative_node_hours\":" + num(c.negative_hours);
    out += ",\"metrics\":" + metrics_json(det::metrics(c));
    out += ",\"by_fault\":{";
    for (std::size_t k = 0; k < det::kFaultKindCount; ++k) {
        const auto& kc = c.by_kind.at(k);
        out += k == 0 ? "" : ",";
        out += json_quote(det::to_string(static_cast<det::FaultKind>(k)));
        out += ":{\"episodes\":" + std::to_string(kc.episodes);
        out += ",\"detected\":" + std::to_string(kc.detected);
        out += ",\"metrics\":" + metrics_json(det::kind_metrics(kc)) + "}";
    }
    out += "},\"by_rule\":{";
    bool first = true;
    for (const auto& [rule, rc] : c.by_rule) {
        out += first ? "" : ",";
        first = false;
        out += json_quote(rule);
        out += ":{\"alerts\":" + std::to_string(rc.alerts);
        out += ",\"matched\":" + std::to_string(rc.matched);
        out += ",\"false_alerts\":" + std::to_string(rc.false_alerts) + "}";
    }
    out += "}}";
    return out;
}

std::string learned_json(const det::LearnedTuning& t) {
    const auto& m = t.model;
    const auto secs = [](std::chrono::milliseconds d) {
        return json_number(static_cast<double>(d.count()) / 1000.0);
    };
    double quantile = 0.0;
    for (const auto& row : t.grid) {
        if (row.chosen) {
            quantile = row.quantile;
        }
    }
    std::string out = "{";
    out += "\"training_vectors\":" + std::to_string(m.training_vectors);
    if (m.knn) {
        out += ",\"k\":" + std::to_string(m.knn->k()) +
               ",\"reference_vectors\":" + std::to_string(m.knn->reference_size());
    }
    if (m.perceptron) {
        out += ",\"positive_vectors\":" + std::to_string(m.positive_vectors) +
               ",\"training_mistakes\":" + std::to_string(m.perceptron->mistakes()) +
               ",\"bias\":" + num(m.perceptron->bias()) + ",\"weights\":[";
        for (std::size_t i = 0; i < m.perceptron->weights().size(); ++i) {
            out += (i == 0 ? "" : ",") + num(m.perceptron->weights()[i]);
        }
        out += "]";
    }
    out += ",\"normal_median_score\":" + num(m.normal_median_score);
    out += R"(,"chosen":{"quantile":)" + num(quantile) + ",\"trigger\":" + num(m.alerter.trigger) +
           ",\"clear\":" + num(m.alerter.clear) +
           ",\"sustain_seconds\":" + secs(m.alerter.sustain) +
           ",\"clear_for_seconds\":" + secs(m.alerter.clear_for) +
           ",\"cooldown_seconds\":" + secs(m.alerter.cooldown) + "}}";
    return out;
}

std::string range_text(const SeedRange& r) {
    return std::to_string(r.first) + "-" + std::to_string(r.last);
}

void metrics_row(std::string& out, const std::string& label, const det::EvaluationCounts& c) {
    const auto m = det::metrics(c);
    out += "| " + label + " | " + std::to_string(c.alerts) + " | " +
           std::to_string(c.false_alerts) + " | " + std::to_string(c.detected) + "/" +
           std::to_string(c.episodes) + " | " + pct(m.precision) + " | " + pct(m.recall) + " | " +
           fixed(m.f1, 3) + " | " + fixed(m.false_alerts_per_hour, 2) + " | " +
           fixed(m.latency_median_seconds, 0, " s") + " | " +
           fixed(m.latency_p95_seconds, 0, " s") + " |\n";
}

void kind_row(std::string& out, const std::string& label, const det::EvaluationCounts& c) {
    out += "| " + label + " |";
    for (std::size_t k = 0; k < det::kFaultKindCount; ++k) {
        const auto& kc = c.by_kind.at(k);
        out += " " + std::to_string(kc.detected) + "/" + std::to_string(kc.episodes) + " (" +
               fixed(det::kind_metrics(kc).latency_median_seconds, 0, " s") + ") |";
    }
    out += "\n";
}

}  // namespace

void append_alert_rows(std::vector<AlertRow>& rows, const det::Scenario& scenario,
                       const std::vector<det::Detection>& detections,
                       const det::MatchConfig& match) {
    for (const auto& d : detections) {
        if (d.kind != det::DetectionKind::kFired) {
            continue;
        }
        AlertRow row;
        row.seed = scenario.config.seed;
        row.rule_id = d.rule_id;
        row.node = d.node;
        row.severity = std::string{det::to_string(d.severity)};
        row.at_seconds = seconds_since(scenario.config.start, d.at);
        row.matched = "false_alert";
        for (const auto& e : scenario.episodes) {
            if (e.node == d.node && d.at >= e.onset && d.at <= e.end + match.grace) {
                row.matched = std::string{det::to_string(e.kind)};
                break;
            }
        }
        rows.push_back(std::move(row));
    }
}

std::string summary_json(const EvaluationReport& report) {
    const auto& schema = features::default_feature_schema();
    std::string out = "{\n";
    out += "\"tool\":\"faulttwin-detect evaluate\",\n";
    out += "\"data\":\"synthetic scenarios (illustrative, not measurements of a database)\",\n";
    out += R"("feature_schema":{"version":)" + json_quote(schema.version()) +
           ",\"fingerprint\":" + json_quote(std::to_string(schema.fingerprint())) +
           ",\"features\":" + std::to_string(schema.size()) + "},\n";
    out += "\"rule_set\":" + json_quote(det::default_rule_set().version) + ",\n";
    out += "\"grace_seconds\":" + std::to_string(report.options.grace_seconds) + ",\n";
    out += R"("validation":{"seeds":)" + json_quote(range_text(report.options.validation)) +
           ",\"episodes\":" + std::to_string(report.validation_episodes) + "},\n";
    out += R"("chosen_params":{"tuned_rules":)" + params_json(report.tuned.params) +
           ",\"naive_threshold\":" + params_json(report.naive.params) + "},\n";
    out += R"("learned":{"version":)" + json_quote(det::kLearnedDetectorsVersion) +
           ",\"train_seeds\":" + json_quote(range_text(report.learned_train)) +
           ",\"threshold_seeds\":" + json_quote(range_text(report.learned_tune)) +
           ",\"features\":[";
    for (std::size_t i = 0; i < det::learned_features().size(); ++i) {
        out += (i == 0 ? "" : ",") + json_quote(features::feature_name(det::learned_features()[i]));
    }
    out += "],\n \"knn_distance\":" + learned_json(report.knn) +
           ",\n \"perceptron\":" + learned_json(report.perceptron) + "},\n";
    out += "\"splits\":{";
    bool first = true;
    for (const auto& split : report.splits) {
        out += first ? "\n" : ",\n";
        first = false;
        out += json_quote(split.name) + ":{\"seeds\":" + json_quote(range_text(split.seeds)) +
               ",\"intensity\":" + json_quote(split.shifted ? "0.5-0.8" : "0.8-1.2") +
               ",\"observations\":" + std::to_string(split.observations) +
               ",\"feature_vectors\":" + std::to_string(split.vectors) +
               ",\"distractors\":" + std::to_string(split.distractors) +
               ",\n \"tuned_rules\":" + counts_json(split.tuned) +
               ",\n \"naive_threshold\":" + counts_json(split.naive) +
               ",\n \"knn_distance\":" + counts_json(split.knn) +
               ",\n \"perceptron\":" + counts_json(split.perceptron) + "}";
    }
    out += "\n}\n}\n";
    return out;
}

std::string summary_markdown(const EvaluationReport& report) {
    std::string out = "# faulttwin-detect evaluate\n\n";
    out += "Synthetic scenarios only (illustrative values, not database measurements).\n";
    out += "Validation seeds " + range_text(report.options.validation) + " (" +
           std::to_string(report.validation_episodes) +
           " episodes) were used for tuning; each test split was evaluated once.\n";
    out += "Alert matching: onset <= alert <= end + " +
           std::to_string(report.options.grace_seconds) + " s, same node.\n\n";

    out +=
        "## Chosen parameters (validation)\n\n| Rule | Tuned trigger | Tuned sustain | Naive "
        "trigger |\n|---|---|---|---|\n";
    for (const auto& row : report.tuned.grid) {
        if (!row.chosen) {
            continue;
        }
        std::string naive_trigger = "N/A";
        for (const auto& n : report.naive.grid) {
            if (n.chosen && n.rule_id == row.rule_id) {
                naive_trigger = json_number(n.trigger);
            }
        }
        out += "| " + row.rule_id + " | " + json_number(row.trigger) + " | " +
               json_number(static_cast<double>(row.sustain.count()) / 1000.0) + " s | " +
               naive_trigger + " |\n";
    }

    out += "\nLearned detectors: trained on seeds " + range_text(report.learned_train) +
           ", alert threshold and sustain chosen on seeds " + range_text(report.learned_tune) +
           ".\n\n| Detector | Trigger (normal-score quantile) | Clear | Sustain "
           "|\n|---|---|---|---|\n";
    for (const auto* t : {&report.knn, &report.perceptron}) {
        double quantile = 0.0;
        for (const auto& row : t->grid) {
            if (row.chosen) {
                quantile = row.quantile;
            }
        }
        out += "| " + t->model.alerter.detector + " | " + decimals(t->model.alerter.trigger, 3) +
               " (q " + json_number(quantile) + ") | " + decimals(t->model.alerter.clear, 3) +
               " | " + json_number(static_cast<double>(t->model.alerter.sustain.count()) / 1000.0) +
               " s |\n";
    }

    for (const auto& split : report.splits) {
        out += "\n## Split `" + split.name + "` (seeds " + range_text(split.seeds) +
               ", intensity " + (split.shifted ? "0.5-0.8" : "0.8-1.2") + ")\n\n";
        out +=
            "| Detector | Alerts | False alerts | Detected | Precision | Recall | F1 | False "
            "alerts/h | Latency p50 | Latency p95 |\n|---|---|---|---|---|---|---|---|---|---|\n";
        metrics_row(out, "tuned rules", split.tuned);
        metrics_row(out, "naive threshold", split.naive);
        metrics_row(out, "k-NN distance", split.knn);
        metrics_row(out, "averaged perceptron", split.perceptron);
        out += "\nDetected per fault class (latency p50):\n\n| Detector |";
        for (std::size_t k = 0; k < det::kFaultKindCount; ++k) {
            out += " " + std::string{det::to_string(static_cast<det::FaultKind>(k))} + " |";
        }
        out += "\n|---|---|---|---|---|\n";
        kind_row(out, "tuned rules", split.tuned);
        kind_row(out, "naive threshold", split.naive);
        kind_row(out, "k-NN distance", split.knn);
        kind_row(out, "averaged perceptron", split.perceptron);
    }
    return out;
}

std::string tuning_csv(const det::TuningResult& tuning) {
    std::string out =
        "rule_id,trigger,sustain_seconds,episodes,detected,matched_alerts,false_alerts,"
        "precision,recall,f1,false_alerts_per_hour,latency_median_seconds,chosen\n";
    for (const auto& r : tuning.grid) {
        out += r.rule_id + "," + json_number(r.trigger) + "," +
               json_number(static_cast<double>(r.sustain.count()) / 1000.0) + "," +
               std::to_string(r.episodes) + "," + std::to_string(r.detected) + "," +
               std::to_string(r.matched) + "," + std::to_string(r.false_alerts) + "," +
               num(r.metrics.precision) + "," + num(r.metrics.recall) + "," + num(r.metrics.f1) +
               "," + num(r.metrics.false_alerts_per_hour) + "," +
               num(r.metrics.latency_median_seconds) + "," + (r.chosen ? "1" : "0") + "\n";
    }
    return out;
}

std::string learned_tuning_csv(const det::LearnedTuning& tuning) {
    std::string out =
        "detector,quantile,trigger,sustain_seconds,episodes,detected,matched_alerts,false_alerts,"
        "precision,recall,f1,false_alerts_per_hour,latency_median_seconds,chosen\n";
    for (const auto& r : tuning.grid) {
        out += tuning.model.alerter.detector + "," + json_number(r.quantile) + "," +
               json_number(r.trigger) + "," +
               json_number(static_cast<double>(r.sustain.count()) / 1000.0) + "," +
               std::to_string(r.episodes) + "," + std::to_string(r.detected) + "," +
               std::to_string(r.matched) + "," + std::to_string(r.false_alerts) + "," +
               num(r.metrics.precision) + "," + num(r.metrics.recall) + "," + num(r.metrics.f1) +
               "," + num(r.metrics.false_alerts_per_hour) + "," +
               num(r.metrics.latency_median_seconds) + "," + (r.chosen ? "1" : "0") + "\n";
    }
    return out;
}

std::string alerts_csv(const std::vector<AlertRow>& rows) {
    std::string out = "seed,rule_id,node,severity,at_seconds,matched\n";
    for (const auto& r : rows) {
        out += std::to_string(r.seed) + "," + r.rule_id + "," + r.node + "," + r.severity + "," +
               json_number(r.at_seconds) + "," + r.matched + "\n";
    }
    return out;
}

std::string scenario_text(const det::Scenario& scenario,
                          const std::vector<det::Detection>& detections, bool jsonl) {
    std::string out;
    const auto start = scenario.config.start;
    if (jsonl) {
        for (const auto& e : scenario.episodes) {
            out += R"({"type":"episode","node":)" + json_quote(e.node) +
                   ",\"fault\":" + json_quote(det::to_string(e.kind)) +
                   ",\"onset\":" + json_quote(det::iso8601(e.onset)) +
                   ",\"end\":" + json_quote(det::iso8601(e.end)) +
                   ",\"intensity\":" + json_number(e.intensity) + "}\n";
        }
        for (const auto& d : scenario.distractors) {
            out += R"({"type":"distractor","node":)" + json_quote(d.node) +
                   ",\"kind\":" + json_quote(det::to_string(d.kind)) +
                   ",\"start\":" + json_quote(det::iso8601(d.start)) +
                   ",\"end\":" + json_quote(det::iso8601(d.end)) + "}\n";
        }
        for (const auto& d : detections) {
            out += det::detection_json(d) + "\n";
        }
        return out;
    }
    out += "scenario seed " + std::to_string(scenario.config.seed) + ", " +
           std::to_string(scenario.observations.size()) + " observations\n";
    for (const auto& e : scenario.episodes) {
        out += "  episode    " + e.node + " " + std::string{det::to_string(e.kind)} +
               " t=" + json_number(seconds_since(start, e.onset)) + ".." +
               json_number(seconds_since(start, e.end)) + " s\n";
    }
    for (const auto& d : scenario.distractors) {
        out += "  distractor " + d.node + " " + std::string{det::to_string(d.kind)} +
               " t=" + json_number(seconds_since(start, d.start)) + ".." +
               json_number(seconds_since(start, d.end)) + " s\n";
    }
    for (const auto& d : detections) {
        out += "  t=" + json_number(seconds_since(start, d.at)) + " s " + det::detection_text(d) +
               "\n";
    }
    return out;
}

}  // namespace faulttwin::detect
