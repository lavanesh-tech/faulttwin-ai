#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "faulttwin/storage/incident.hpp"

namespace faulttwin::storage {

enum class StoreErrorKind : std::uint8_t {
    kUnavailable,  ///< backend unreachable or timed out; the caller may retry later
    kNotFound,     ///< no incident with that id
    kConflict,     ///< state does not allow it (already resolved, already acknowledged, an
                   ///< incident for this rule and node is already open)
    kInvalid,      ///< the request failed validation
    kCapacity,     ///< bounded store is full of open incidents
    kInternal,     ///< corrupt row, unexpected result shape
};
[[nodiscard]] std::string_view to_string(StoreErrorKind kind) noexcept;

struct StoreError {
    StoreErrorKind kind{StoreErrorKind::kInternal};
    std::string message;  ///< one line, safe to log and to return to API clients
};

template <typename T>
using StoreResult = std::variant<T, StoreError>;
struct Done {};

/// Filters for list(). Every filter is optional; results are ordered by id descending
/// (newest first) and paged with a keyset cursor (`before_id`), which stays correct while new
/// incidents are inserted, unlike OFFSET paging.
struct IncidentQuery {
    std::optional<IncidentState> state;
    std::optional<std::string> node;
    std::optional<std::string> rule_id;
    std::optional<Severity> severity;
    std::optional<Timestamp> opened_since;  ///< opened_at >= since
    std::optional<Timestamp> opened_until;  ///< opened_at < until
    std::optional<std::uint64_t> before_id;
    std::size_t limit{50};
};
inline constexpr std::size_t kMaxPageSize = 500;

struct IncidentPage {
    std::vector<Incident> items;
    std::optional<std::uint64_t> next_before_id;  ///< set when more items may follow
};

struct StoreStats {
    std::uint64_t open{0};
    std::uint64_t resolved{0};
};

/// Persistence for incidents. Implementations are thread-safe: the monitor loop writes while
/// API threads read. Every method reports failure as a value; none throws for an expected
/// failure (unreachable backend, missing id, conflict).
class IncidentStore {
  public:
    IncidentStore() = default;
    IncidentStore(const IncidentStore&) = delete;
    IncidentStore& operator=(const IncidentStore&) = delete;
    IncidentStore(IncidentStore&&) = delete;
    IncidentStore& operator=(IncidentStore&&) = delete;
    virtual ~IncidentStore() = default;

    /// Opens an incident and returns its id. kConflict if one is already open for the same
    /// (rule_id, node).
    [[nodiscard]] virtual StoreResult<std::uint64_t> open(const NewIncident& incident) = 0;
    /// Records an escalation: raises severity and appends the event. kConflict if resolved.
    [[nodiscard]] virtual StoreResult<Done> escalate(std::uint64_t id,
                                                     const IncidentEvent& event) = 0;
    /// Resolves an open incident. kConflict if already resolved.
    [[nodiscard]] virtual StoreResult<Done> resolve(std::uint64_t id, const IncidentEvent& event,
                                                    Resolution resolution) = 0;
    /// Closes every open incident as kMonitorRestarted at `at` and returns how many.
    [[nodiscard]] virtual StoreResult<std::uint64_t> close_orphans(Timestamp at) = 0;
    /// Records a human acknowledgement. kConflict if already acknowledged (decisions are not
    /// silently overwritten).
    [[nodiscard]] virtual StoreResult<Incident> acknowledge(std::uint64_t id,
                                                            const Acknowledgement& ack) = 0;

    [[nodiscard]] virtual StoreResult<Incident> get(std::uint64_t id) = 0;
    [[nodiscard]] virtual StoreResult<IncidentPage> list(const IncidentQuery& query) = 0;
    [[nodiscard]] virtual StoreResult<StoreStats> stats() = 0;
    /// Cheap reachability check used by /readyz.
    [[nodiscard]] virtual StoreResult<Done> ping() = 0;
    /// "memory" or "postgres", for logs and /api/v1/version.
    [[nodiscard]] virtual std::string_view backend() const noexcept = 0;
};

/// Validates a query against the limits (1 <= limit <= kMaxPageSize, since < until, filter
/// strings within kMaxIdentifierBytes). Empty string means valid.
[[nodiscard]] std::string validate_query(const IncidentQuery& query);

}  // namespace faulttwin::storage
