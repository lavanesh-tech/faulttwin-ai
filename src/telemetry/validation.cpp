#include "faulttwin/telemetry/validation.hpp"

#include <cmath>

namespace faulttwin::telemetry {

namespace detail {
// The only code allowed to construct a TelemetrySample (see the friend
// declaration in telemetry_sample.hpp).
struct SampleFactory {
    static TelemetrySample make(NodeId node_id, const RawTelemetrySample& raw) {
        return TelemetrySample{std::move(node_id),       raw.role,
                               raw.collected_at,         raw.cpu_usage_percent,
                               raw.memory_usage_percent, raw.disk_usage_percent,
                               raw.replication_lag_ms};
    }
};
}  // namespace detail

namespace {

constexpr double kMinPercent = 0.0;
constexpr double kMaxPercent = 100.0;

constexpr ValidationCode to_validation_code(NodeIdError error) noexcept {
    switch (error) {
        case NodeIdError::kEmpty:
            return ValidationCode::kEmpty;
        case NodeIdError::kTooLong:
            return ValidationCode::kTooLong;
        case NodeIdError::kInvalidFirstCharacter:
            return ValidationCode::kInvalidFirstCharacter;
        case NodeIdError::kInvalidCharacter:
            return ValidationCode::kInvalidCharacter;
    }
    return ValidationCode::kInvalidCharacter;
}

}  // namespace

std::string_view to_string(ReplicationLagState state) noexcept {
    switch (state) {
        case ReplicationLagState::kNotApplicable:
            return "not_applicable";
        case ReplicationLagState::kNotMeasured:
            return "not_measured";
        case ReplicationLagState::kMeasured:
            return "measured";
    }
    return "invalid";
}

ReplicationLagState TelemetrySample::replication_lag_state() const noexcept {
    if (role_ != NodeRole::kReplica) {
        return ReplicationLagState::kNotApplicable;
    }
    return replication_lag_ms_.has_value() ? ReplicationLagState::kMeasured
                                           : ReplicationLagState::kNotMeasured;
}

std::string_view to_string(Field field) noexcept {
    switch (field) {
        case Field::kNodeId:
            return "node_id";
        case Field::kRole:
            return "role";
        case Field::kCollectedAt:
            return "collected_at";
        case Field::kCpuUsagePercent:
            return "cpu_usage_percent";
        case Field::kMemoryUsagePercent:
            return "memory_usage_percent";
        case Field::kDiskUsagePercent:
            return "disk_usage_percent";
        case Field::kReplicationLagMs:
            return "replication_lag_ms";
    }
    return "invalid_field";
}

std::string_view to_string(ValidationCode code) noexcept {
    switch (code) {
        case ValidationCode::kEmpty:
            return "empty";
        case ValidationCode::kTooLong:
            return "too_long";
        case ValidationCode::kInvalidFirstCharacter:
            return "invalid_first_character";
        case ValidationCode::kInvalidCharacter:
            return "invalid_character";
        case ValidationCode::kInvalidEnumValue:
            return "invalid_enum_value";
        case ValidationCode::kTimestampUnset:
            return "timestamp_unset";
        case ValidationCode::kTimestampInFuture:
            return "timestamp_in_future";
        case ValidationCode::kNotFinite:
            return "not_finite";
        case ValidationCode::kOutOfRange:
            return "out_of_range";
        case ValidationCode::kNegative:
            return "negative";
        case ValidationCode::kReplicationLagNotApplicable:
            return "replication_lag_not_applicable";
    }
    return "invalid_code";
}

std::string_view to_string(Freshness freshness) noexcept {
    switch (freshness) {
        case Freshness::kFresh:
            return "fresh";
        case Freshness::kStale:
            return "stale";
    }
    return "invalid";
}

std::optional<ValidationCode> check_percentage(double value) noexcept {
    if (!std::isfinite(value)) {
        return ValidationCode::kNotFinite;
    }
    if (value < kMinPercent || value > kMaxPercent) {
        return ValidationCode::kOutOfRange;
    }
    return std::nullopt;
}

std::optional<ValidationCode> check_timestamp(Timestamp collected_at, Timestamp now,
                                              const ValidationPolicy& policy) noexcept {
    if (collected_at <= Timestamp{}) {
        return ValidationCode::kTimestampUnset;
    }
    if (collected_at > now + policy.max_future_skew) {
        return ValidationCode::kTimestampInFuture;
    }
    return std::nullopt;
}

std::optional<ValidationCode> check_replication_lag(
    NodeRole role, std::optional<double> replication_lag_ms) noexcept {
    if (!replication_lag_ms.has_value()) {
        // Absent is always acceptable: for a replica it means "not measured",
        // for a primary or unknown role it is the only acceptable state.
        return std::nullopt;
    }
    if (role == NodeRole::kPrimary || role == NodeRole::kUnknown) {
        return ValidationCode::kReplicationLagNotApplicable;
    }
    const double lag = *replication_lag_ms;
    if (!std::isfinite(lag)) {
        return ValidationCode::kNotFinite;
    }
    if (lag < 0.0) {
        return ValidationCode::kNegative;
    }
    return std::nullopt;
}

ValidationResult validate_sample(const RawTelemetrySample& raw, Timestamp now,
                                 const ValidationPolicy& policy) {
    ValidationResult result;

    auto record = [&result](Field field, std::optional<ValidationCode> code) {
        if (code.has_value()) {
            result.issues.push_back(ValidationIssue{field, *code});
        }
    };

    std::optional<NodeId> node_id = NodeId::parse(raw.node_id);
    if (!node_id.has_value()) {
        // parse() failed, so check_node_id() is guaranteed to report an error.
        const std::optional<NodeIdError> error = check_node_id(raw.node_id);
        record(Field::kNodeId, to_validation_code(error.value_or(NodeIdError::kInvalidCharacter)));
    }
    if (!is_valid(raw.role)) {
        record(Field::kRole, ValidationCode::kInvalidEnumValue);
    }
    record(Field::kCollectedAt, check_timestamp(raw.collected_at, now, policy));
    record(Field::kCpuUsagePercent, check_percentage(raw.cpu_usage_percent));
    record(Field::kMemoryUsagePercent, check_percentage(raw.memory_usage_percent));
    record(Field::kDiskUsagePercent, check_percentage(raw.disk_usage_percent));
    record(Field::kReplicationLagMs, check_replication_lag(raw.role, raw.replication_lag_ms));

    if (result.issues.empty() && node_id.has_value()) {
        result.sample = detail::SampleFactory::make(std::move(*node_id), raw);
    }
    return result;
}

Freshness classify_freshness(const TelemetrySample& sample, Timestamp now,
                             std::chrono::milliseconds max_age) noexcept {
    return (now - sample.collected_at() > max_age) ? Freshness::kStale : Freshness::kFresh;
}

}  // namespace faulttwin::telemetry
