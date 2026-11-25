#include "faulttwin/telemetry/synthetic.hpp"

#include <array>
#include <limits>
#include <string>

namespace faulttwin::telemetry {
namespace {

/// SplitMix64 (Steele, Lea, Flood 2014). Tiny, fast, fully specified, and
/// identical on every platform, which std::*_distribution is not.
class SplitMix64 {
  public:
    explicit SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

    std::uint64_t next() noexcept {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }

    /// Uniform double in [0, 1) built from the top 53 bits.
    double next_unit() noexcept {
        constexpr double kTwoPowMinus53 = 0x1.0p-53;
        return static_cast<double>(next() >> 11U) * kTwoPowMinus53;
    }

    /// Uniform double in [lo, hi).
    double next_in(double lo, double hi) noexcept { return lo + ((hi - lo) * next_unit()); }

    /// Uniform index in [0, n). The modulo bias is < n / 2^64, negligible here.
    std::size_t next_index(std::size_t n) noexcept {
        return static_cast<std::size_t>(next() % static_cast<std::uint64_t>(n));
    }

  private:
    std::uint64_t state_;
};

constexpr std::array<InjectedDefect, kInjectedDefectCount> kAllDefects{
    InjectedDefect::kEmptyNodeId,
    InjectedDefect::kOverlongNodeId,
    InjectedDefect::kInvalidNodeIdCharacter,
    InjectedDefect::kInvalidRole,
    InjectedDefect::kUnsetTimestamp,
    InjectedDefect::kFutureTimestamp,
    InjectedDefect::kCpuOutOfRange,
    InjectedDefect::kMemoryNaN,
    InjectedDefect::kDiskInfinity,
    InjectedDefect::kNegativeReplicationLag,
    InjectedDefect::kReplicationLagOnPrimary,
};

// An out-of-range underlying value for NodeRole, as corrupted input could produce.
constexpr auto kCorruptRoleValue = std::uint8_t{0xEE};

std::string node_name(std::uint32_t index) {
    return index == 0 ? std::string{"pg-primary"} : "pg-replica-" + std::to_string(index);
}

void apply_defect(InjectedDefect defect, RawTelemetrySample& raw, Timestamp now) {
    switch (defect) {
        case InjectedDefect::kEmptyNodeId:
            raw.node_id.clear();
            break;
        case InjectedDefect::kOverlongNodeId:
            raw.node_id.assign(kMaxNodeIdLength + 1, 'n');
            break;
        case InjectedDefect::kInvalidNodeIdCharacter:
            raw.node_id += "/../etc";
            break;
        case InjectedDefect::kInvalidRole:
            raw.role = static_cast<NodeRole>(kCorruptRoleValue);
            raw.replication_lag_ms.reset();  // keep exactly one issue per defect
            break;
        case InjectedDefect::kUnsetTimestamp:
            raw.collected_at = Timestamp{};
            break;
        case InjectedDefect::kFutureTimestamp:
            raw.collected_at = now + std::chrono::hours{1};
            break;
        case InjectedDefect::kCpuOutOfRange:
            raw.cpu_usage_percent = 100.5;
            break;
        case InjectedDefect::kMemoryNaN:
            raw.memory_usage_percent = std::numeric_limits<double>::quiet_NaN();
            break;
        case InjectedDefect::kDiskInfinity:
            raw.disk_usage_percent = std::numeric_limits<double>::infinity();
            break;
        case InjectedDefect::kNegativeReplicationLag:
            raw.role = NodeRole::kReplica;
            raw.replication_lag_ms = -1.0;
            break;
        case InjectedDefect::kReplicationLagOnPrimary:
            raw.role = NodeRole::kPrimary;
            raw.replication_lag_ms = 5.0;
            break;
    }
}

}  // namespace

std::string_view to_string(InjectedDefect defect) noexcept {
    switch (defect) {
        case InjectedDefect::kEmptyNodeId:
            return "empty_node_id";
        case InjectedDefect::kOverlongNodeId:
            return "overlong_node_id";
        case InjectedDefect::kInvalidNodeIdCharacter:
            return "invalid_node_id_character";
        case InjectedDefect::kInvalidRole:
            return "invalid_role";
        case InjectedDefect::kUnsetTimestamp:
            return "unset_timestamp";
        case InjectedDefect::kFutureTimestamp:
            return "future_timestamp";
        case InjectedDefect::kCpuOutOfRange:
            return "cpu_out_of_range";
        case InjectedDefect::kMemoryNaN:
            return "memory_nan";
        case InjectedDefect::kDiskInfinity:
            return "disk_infinity";
        case InjectedDefect::kNegativeReplicationLag:
            return "negative_replication_lag";
        case InjectedDefect::kReplicationLagOnPrimary:
            return "replication_lag_on_primary";
    }
    return "invalid_defect";
}

ValidationIssue expected_issue(InjectedDefect defect) noexcept {
    switch (defect) {
        case InjectedDefect::kEmptyNodeId:
            return {Field::kNodeId, ValidationCode::kEmpty};
        case InjectedDefect::kOverlongNodeId:
            return {Field::kNodeId, ValidationCode::kTooLong};
        case InjectedDefect::kInvalidNodeIdCharacter:
            return {Field::kNodeId, ValidationCode::kInvalidCharacter};
        case InjectedDefect::kInvalidRole:
            return {Field::kRole, ValidationCode::kInvalidEnumValue};
        case InjectedDefect::kUnsetTimestamp:
            return {Field::kCollectedAt, ValidationCode::kTimestampUnset};
        case InjectedDefect::kFutureTimestamp:
            return {Field::kCollectedAt, ValidationCode::kTimestampInFuture};
        case InjectedDefect::kCpuOutOfRange:
            return {Field::kCpuUsagePercent, ValidationCode::kOutOfRange};
        case InjectedDefect::kMemoryNaN:
            return {Field::kMemoryUsagePercent, ValidationCode::kNotFinite};
        case InjectedDefect::kDiskInfinity:
            return {Field::kDiskUsagePercent, ValidationCode::kNotFinite};
        case InjectedDefect::kNegativeReplicationLag:
            return {Field::kReplicationLagMs, ValidationCode::kNegative};
        case InjectedDefect::kReplicationLagOnPrimary:
            return {Field::kReplicationLagMs, ValidationCode::kReplicationLagNotApplicable};
    }
    return {Field::kNodeId, ValidationCode::kInvalidCharacter};
}

Timestamp synthetic_reference_now(const SyntheticConfig& config) noexcept {
    const auto last_tick = config.samples_per_node == 0 ? 0U : config.samples_per_node - 1U;
    return config.start_time + (config.interval * static_cast<std::int64_t>(last_tick));
}

std::vector<SyntheticSample> generate_synthetic_samples(const SyntheticConfig& config) {
    std::vector<SyntheticSample> samples;
    samples.reserve(static_cast<std::size_t>(config.node_count) * config.samples_per_node);

    SplitMix64 rng{config.seed};
    const Timestamp now = synthetic_reference_now(config);

    // Tick-major order: every node reports once per tick, like a scrape cycle.
    for (std::uint32_t tick = 0; tick < config.samples_per_node; ++tick) {
        const Timestamp collected_at =
            config.start_time + (config.interval * static_cast<std::int64_t>(tick));
        for (std::uint32_t node = 0; node < config.node_count; ++node) {
            RawTelemetrySample raw;
            raw.node_id = node_name(node);
            raw.role = node == 0 ? NodeRole::kPrimary : NodeRole::kReplica;
            raw.collected_at = collected_at;
            raw.cpu_usage_percent = rng.next_in(5.0, 85.0);
            raw.memory_usage_percent = rng.next_in(20.0, 80.0);
            raw.disk_usage_percent = rng.next_in(10.0, 70.0);
            // Always draw, so the stream does not depend on the branch taken.
            const double lag_value = rng.next_in(0.0, 250.0);
            const bool lag_missing = rng.next_unit() < config.replica_lag_missing_fraction;
            if (raw.role == NodeRole::kReplica && !lag_missing) {
                raw.replication_lag_ms = lag_value;
            }

            SyntheticSample sample{std::move(raw), std::nullopt};
            const bool inject = rng.next_unit() < config.invalid_fraction;
            const std::size_t defect_index = rng.next_index(kAllDefects.size());
            if (inject) {
                const InjectedDefect defect = kAllDefects.at(defect_index);
                apply_defect(defect, sample.raw, now);
                sample.defect = defect;
            }
            samples.push_back(std::move(sample));
        }
    }
    return samples;
}

}  // namespace faulttwin::telemetry
