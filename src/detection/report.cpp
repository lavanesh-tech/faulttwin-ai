#include "faulttwin/detection/report.hpp"

#include "faulttwin/core/json.hpp"
#include "faulttwin/core/time.hpp"

namespace faulttwin::detection {

// The JSON and time helpers moved to faulttwin_core in Phase 5; these forward to them so the
// Phase 4 output stays byte-identical.
std::string json_number(std::optional<double> value) {
    return core::json_number(value);
}

std::string json_quote(std::string_view text) {
    return core::json_quote(text);
}

std::string iso8601(Timestamp t) {
    return core::iso8601(t);
}

std::string detection_json(const Detection& d) {
    std::string out = R"({"type":"detection","kind":)";
    out += json_quote(to_string(d.kind));
    out += ",\"rule_set\":";
    out += json_quote(d.rule_set_version);
    out += ",\"rule\":";
    out += json_quote(d.rule_id);
    out += ",\"node\":";
    out += json_quote(d.node);
    out += ",\"severity\":";
    out += json_quote(to_string(d.severity));
    out += ",\"at\":";
    out += json_quote(iso8601(d.at));
    out += ",\"violation_since\":";
    out += json_quote(iso8601(d.violation_since));
    out += ",\"evidence\":{";
    bool first = true;
    for (const auto& e : d.evidence) {
        out += first ? "" : ",";
        first = false;
        out += json_quote(e.feature);
        out += ':';
        out += json_number(e.value);
    }
    out += "}}";
    return out;
}

std::string detection_text(const Detection& d) {
    std::string out = iso8601(d.at);
    out += ' ';
    out += to_string(d.kind);
    out += ' ';
    out += to_string(d.severity);
    out += ' ';
    out += d.rule_id;
    out += " node=";
    out += d.node;
    for (const auto& e : d.evidence) {
        out += ' ';
        out += e.feature;
        out += '=';
        out += json_number(e.value);
    }
    return out;
}

}  // namespace faulttwin::detection
