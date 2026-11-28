#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "faulttwin/detection/rule_engine.hpp"

namespace faulttwin::storage {

using detection::DetectionKind;
using detection::Evidence;
using detection::Severity;
using detection::Timestamp;

enum class IncidentState : std::uint8_t { kOpen, kResolved };
[[nodiscard]] std::string_view to_string(IncidentState state) noexcept;
[[nodiscard]] std::optional<IncidentState> incident_state_from_string(std::string_view t) noexcept;
[[nodiscard]] std::optional<Severity> severity_from_string(std::string_view text) noexcept;

/// Why an incident was closed. kRecovered: the rule engine reported the condition cleared.
/// kMonitorRestarted: the process that owned the alert state stopped before the alert
/// resolved; the incident is closed at the next start because nothing would ever resolve it,
/// and the reason says so rather than pretending the fault recovered.
enum class Resolution : std::uint8_t { kRecovered, kMonitorRestarted };
[[nodiscard]] std::string_view to_string(Resolution resolution) noexcept;
[[nodiscard]] std::optional<Resolution> resolution_from_string(std::string_view t) noexcept;

/// One state change of the alert behind an incident, with the evidence at that moment.
struct IncidentEvent {
    DetectionKind kind{DetectionKind::kFired};
    Severity severity{Severity::kWarning};
    Timestamp at;
    std::vector<Evidence> evidence;
};

/// A human acknowledgement: "I have seen this". It records a decision, it changes nothing in
/// the monitored cluster.
struct Acknowledgement {
    std::string by;
    std::string note;
    Timestamp at;
};

/// An incident is the lifetime of one alert: one (rule, node) from fired to resolved. The
/// rule engine emits at most fired, escalated and resolved for one alert, so `events` holds at
/// most three entries.
struct Incident {
    std::uint64_t id{0};
    std::string rule_set_version;
    std::string feature_schema_version;
    std::string rule_id;
    std::string node;
    Severity severity{Severity::kWarning};
    IncidentState state{IncidentState::kOpen};
    std::optional<Resolution> resolution;
    Timestamp violation_since;
    Timestamp opened_at;
    std::optional<Timestamp> resolved_at;
    Timestamp updated_at;
    std::vector<IncidentEvent> events;
    std::optional<Acknowledgement> acknowledgement;
};

/// Everything needed to open an incident from a kFired detection.
struct NewIncident {
    std::string rule_set_version;
    std::string feature_schema_version;
    std::string rule_id;
    std::string node;
    Severity severity{Severity::kWarning};
    Timestamp violation_since;
    Timestamp opened_at;
    std::vector<Evidence> evidence;
};

// ---- Field limits shared by every store and by API validation -----------------------
inline constexpr std::size_t kMaxIdentifierBytes = 128;  ///< versions, rule ids, nodes
inline constexpr std::size_t kMaxAckByBytes = 64;
inline constexpr std::size_t kMaxAckNoteBytes = 1024;
inline constexpr std::size_t kMaxEvidenceItems = 64;
inline constexpr std::size_t kMaxEventsPerIncident = 3;

/// Validates a NewIncident: non-empty identifiers within limits, no control characters,
/// at most kMaxEvidenceItems evidence entries, violation_since <= opened_at. An empty string
/// means valid; otherwise the first problem.
[[nodiscard]] std::string validate_new_incident(const NewIncident& incident);
/// Validates an acknowledgement's `by` (1-64 bytes, printable) and `note` (0-1024 bytes, no
/// control characters except newline and tab).
[[nodiscard]] std::string validate_acknowledgement(const Acknowledgement& ack);

/// Evidence as a compact JSON object {"feature":value|null,...}, the format used for the
/// API and for the meta-db jsonb column.
[[nodiscard]] std::string evidence_json(const std::vector<Evidence>& evidence);
/// Inverse of evidence_json. Values must be numbers or null.
[[nodiscard]] std::optional<std::vector<Evidence>> evidence_from_json(std::string_view json);

}  // namespace faulttwin::storage
