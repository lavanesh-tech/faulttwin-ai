#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "faulttwin/telemetry/telemetry_sample.hpp"

namespace faulttwin::telemetry {

/// Field of RawTelemetrySample that a validation issue refers to.
enum class Field : std::uint8_t {
    kNodeId,
    kRole,
    kCollectedAt,
    kCpuUsagePercent,
    kMemoryUsagePercent,
    kDiskUsagePercent,
    kReplicationLagMs,
};

enum class ValidationCode : std::uint8_t {
    kEmpty,
    kTooLong,
    kInvalidFirstCharacter,
    kInvalidCharacter,
    kInvalidEnumValue,
    kTimestampUnset,     ///< at or before the Unix epoch (default-constructed)
    kTimestampInFuture,  ///< later than now + ValidationPolicy::max_future_skew
    kNotFinite,          ///< NaN or +/- infinity
    kOutOfRange,
    kNegative,
    kReplicationLagNotApplicable,  ///< lag reported for a primary or unknown-role node
};

[[nodiscard]] std::string_view to_string(Field field) noexcept;
[[nodiscard]] std::string_view to_string(ValidationCode code) noexcept;

struct ValidationIssue {
    Field field;
    ValidationCode code;

    friend bool operator==(const ValidationIssue&, const ValidationIssue&) = default;
};

/// Explicit, injectable validation policy. Validation never reads a clock:
/// the caller passes `now`, which keeps validation deterministic and testable.
struct ValidationPolicy {
    /// Collector clocks drift; samples up to this far in the future are accepted.
    std::chrono::milliseconds max_future_skew{std::chrono::seconds{5}};
};

/// Result of validating one raw sample.
/// Invariant: `sample` is engaged if and only if `issues` is empty.
struct ValidationResult {
    std::optional<TelemetrySample> sample;
    std::vector<ValidationIssue> issues;

    [[nodiscard]] bool ok() const noexcept { return sample.has_value(); }
};

// ---- Individual checks (pure, noexcept, used by validate_sample) -------------

/// Percentages must be finite and within [0, 100].
[[nodiscard]] std::optional<ValidationCode> check_percentage(double value) noexcept;

[[nodiscard]] std::optional<ValidationCode> check_timestamp(
    Timestamp collected_at, Timestamp now, const ValidationPolicy& policy) noexcept;

/// Role-specific replication lag semantics:
///  - primary / unknown role: lag must be absent (kReplicationLagNotApplicable otherwise)
///  - replica: lag may be absent (not measured); if present, finite and >= 0
///  - invalid role: role-specific rules cannot be applied; only finiteness and
///    sign are checked (the role itself is reported as an issue separately)
[[nodiscard]] std::optional<ValidationCode> check_replication_lag(
    NodeRole role, std::optional<double> replication_lag_ms) noexcept;

/// Validates every field and reports at most one issue per field, in Field
/// declaration order. All fields are checked (no early exit) so callers see
/// every problem at once.
[[nodiscard]] ValidationResult validate_sample(const RawTelemetrySample& raw, Timestamp now,
                                               const ValidationPolicy& policy = {});

// ---- Freshness (a property of valid data, not a validation failure) ---------

enum class Freshness : std::uint8_t { kFresh, kStale };

[[nodiscard]] std::string_view to_string(Freshness freshness) noexcept;

/// A sample is stale when now - collected_at > max_age. Samples slightly in the
/// future (within the allowed skew) are fresh.
[[nodiscard]] Freshness classify_freshness(const TelemetrySample& sample, Timestamp now,
                                           std::chrono::milliseconds max_age) noexcept;

}  // namespace faulttwin::telemetry
