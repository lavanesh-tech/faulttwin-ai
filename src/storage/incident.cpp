#include "faulttwin/storage/incident.hpp"

#include <algorithm>
#include <array>

#include "faulttwin/core/json.hpp"
#include "faulttwin/storage/incident_store.hpp"

namespace faulttwin::storage {

std::string_view to_string(IncidentState state) noexcept {
    return state == IncidentState::kOpen ? "open" : "resolved";
}

std::optional<IncidentState> incident_state_from_string(std::string_view t) noexcept {
    if (t == "open") {
        return IncidentState::kOpen;
    }
    if (t == "resolved") {
        return IncidentState::kResolved;
    }
    return std::nullopt;
}

std::optional<Severity> severity_from_string(std::string_view text) noexcept {
    if (text == detection::to_string(Severity::kWarning)) {
        return Severity::kWarning;
    }
    if (text == detection::to_string(Severity::kCritical)) {
        return Severity::kCritical;
    }
    return std::nullopt;
}

std::string_view to_string(Resolution resolution) noexcept {
    return resolution == Resolution::kRecovered ? "recovered" : "monitor_restarted";
}

std::optional<Resolution> resolution_from_string(std::string_view t) noexcept {
    if (t == "recovered") {
        return Resolution::kRecovered;
    }
    if (t == "monitor_restarted") {
        return Resolution::kMonitorRestarted;
    }
    return std::nullopt;
}

std::string_view to_string(StoreErrorKind kind) noexcept {
    switch (kind) {
        case StoreErrorKind::kUnavailable:
            return "unavailable";
        case StoreErrorKind::kNotFound:
            return "not_found";
        case StoreErrorKind::kConflict:
            return "conflict";
        case StoreErrorKind::kInvalid:
            return "invalid";
        case StoreErrorKind::kCapacity:
            return "capacity";
        case StoreErrorKind::kInternal:
            return "internal";
    }
    return "internal";
}

namespace {

bool has_control(std::string_view text, bool allow_newline_tab) {
    return std::ranges::any_of(text, [allow_newline_tab](const char c) {
        const auto u = static_cast<unsigned char>(c);
        const bool control = u < 0x20U || u == 0x7FU;
        const bool allowed = allow_newline_tab && (c == '\n' || c == '\t');
        return control && !allowed;
    });
}

std::string check_identifier(std::string_view name, std::string_view value) {
    if (value.empty()) {
        return std::string(name) + " is empty";
    }
    if (value.size() > kMaxIdentifierBytes) {
        return std::string(name) + " is longer than " + std::to_string(kMaxIdentifierBytes) +
               " bytes";
    }
    if (has_control(value, false)) {
        return std::string(name) + " contains control characters";
    }
    return {};
}

}  // namespace

std::string validate_new_incident(const NewIncident& incident) {
    const std::array<std::pair<std::string_view, const std::string*>, 4> fields{{
        {"rule_set_version", &incident.rule_set_version},
        {"feature_schema_version", &incident.feature_schema_version},
        {"rule_id", &incident.rule_id},
        {"node", &incident.node},
    }};
    for (const auto& [name, value] : fields) {
        if (auto problem = check_identifier(name, *value); !problem.empty()) {
            return problem;
        }
    }
    if (incident.evidence.size() > kMaxEvidenceItems) {
        return "more than " + std::to_string(kMaxEvidenceItems) + " evidence items";
    }
    for (const auto& e : incident.evidence) {
        if (auto problem = check_identifier("evidence feature", e.feature); !problem.empty()) {
            return problem;
        }
    }
    if (incident.violation_since > incident.opened_at) {
        return "violation_since is after opened_at";
    }
    return {};
}

std::string validate_acknowledgement(const Acknowledgement& ack) {
    if (ack.by.empty() || ack.by.size() > kMaxAckByBytes) {
        return "'by' must be 1-" + std::to_string(kMaxAckByBytes) + " bytes";
    }
    if (has_control(ack.by, false)) {
        return "'by' contains control characters";
    }
    if (ack.note.size() > kMaxAckNoteBytes) {
        return "'note' is longer than " + std::to_string(kMaxAckNoteBytes) + " bytes";
    }
    if (has_control(ack.note, true)) {
        return "'note' contains control characters";
    }
    return {};
}

std::string validate_query(const IncidentQuery& query) {
    if (query.limit < 1 || query.limit > kMaxPageSize) {
        return "limit must be between 1 and " + std::to_string(kMaxPageSize);
    }
    if (query.opened_since && query.opened_until && *query.opened_since >= *query.opened_until) {
        return "since must be earlier than until";
    }
    if (query.node) {
        if (auto p = check_identifier("node", *query.node); !p.empty()) {
            return p;
        }
    }
    if (query.rule_id) {
        if (auto p = check_identifier("rule", *query.rule_id); !p.empty()) {
            return p;
        }
    }
    return {};
}

std::string evidence_json(const std::vector<Evidence>& evidence) {
    std::string out = "{";
    bool first = true;
    for (const auto& e : evidence) {
        out += first ? "" : ",";
        first = false;
        out += core::json_quote(e.feature);
        out += ':';
        out += core::json_number(e.value);
    }
    out += '}';
    return out;
}

std::optional<std::vector<Evidence>> evidence_from_json(std::string_view json) {
    auto parsed = core::parse_json(json);
    const auto* value = std::get_if<core::JsonValue>(&parsed);
    if (value == nullptr || value->as_object() == nullptr) {
        return std::nullopt;
    }
    std::vector<Evidence> out;
    for (const auto& [name, member] : *value->as_object()) {
        if (member.is_null()) {
            out.push_back({name, std::nullopt});
        } else if (const auto* number = member.as_number()) {
            out.push_back({name, *number});
        } else {
            return std::nullopt;
        }
    }
    return out;
}

}  // namespace faulttwin::storage
