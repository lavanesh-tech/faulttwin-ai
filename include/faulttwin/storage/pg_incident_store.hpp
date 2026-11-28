#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

#include "faulttwin/collectors/sql.hpp"
#include "faulttwin/storage/incident_store.hpp"

namespace faulttwin::storage {

/// Creates a fresh session to the meta-db (a real LibpqSession in production, a fake in tests).
using SessionFactory =
    std::function<std::variant<std::unique_ptr<collectors::SqlSession>, collectors::SqlError>()>;

/// The schema version this build expects. migrate() brings an older schema up to it and
/// refuses to run against a newer one.
inline constexpr int kMetaDbSchemaVersion = 1;

/// Incident store in FaultTwin's own PostgreSQL ("meta-db"), separate from the monitored
/// cluster so that injected faults never touch the observer's records.
///
/// Every write is one SQL statement (data-modifying CTEs), so it is atomic without an explicit
/// transaction, and every read is one statement, so it sees one consistent snapshot. All values
/// travel as bind parameters. Times are stored as timestamptz at microsecond precision.
///
/// One connection, guarded by a mutex: calls are serialised. A connection-level failure or a
/// client deadline drops the connection and returns kUnavailable; the next call reconnects.
/// Limitation (Phase 5): API reads and recorder writes queue behind each other on that one
/// connection, which is fine at the lab's request rates and is revisited in Phase 11.
class PgIncidentStore final : public IncidentStore {
  public:
    explicit PgIncidentStore(SessionFactory factory);

    /// Creates or upgrades the schema inside one transaction under an advisory lock, so two
    /// starting processes cannot race. kInvalid if the database is newer than this build.
    [[nodiscard]] StoreResult<int> migrate();

    [[nodiscard]] StoreResult<std::uint64_t> open(const NewIncident& incident) override;
    [[nodiscard]] StoreResult<Done> escalate(std::uint64_t id, const IncidentEvent& event) override;
    [[nodiscard]] StoreResult<Done> resolve(std::uint64_t id, const IncidentEvent& event,
                                            Resolution resolution) override;
    [[nodiscard]] StoreResult<std::uint64_t> close_orphans(Timestamp at) override;
    [[nodiscard]] StoreResult<Incident> acknowledge(std::uint64_t id,
                                                    const Acknowledgement& ack) override;
    [[nodiscard]] StoreResult<Incident> get(std::uint64_t id) override;
    [[nodiscard]] StoreResult<IncidentPage> list(const IncidentQuery& query) override;
    [[nodiscard]] StoreResult<StoreStats> stats() override;
    [[nodiscard]] StoreResult<Done> ping() override;
    [[nodiscard]] std::string_view backend() const noexcept override { return "postgres"; }

  private:
    using Params = std::vector<std::optional<std::string>>;
    /// Runs one statement on the (re)connected session. Connection-level failures reset the
    /// session and come back as kUnavailable.
    StoreResult<collectors::SqlTable> run(std::string_view sql, const Params& params);
    StoreResult<Done> append_event(std::uint64_t id, const IncidentEvent& event,
                                   std::optional<Resolution> resolution);
    StoreResult<Incident> get_locked(std::uint64_t id);

    SessionFactory factory_;
    std::mutex mutex_;
    std::unique_ptr<collectors::SqlSession> session_;
};

/// Maps one row of the incident SELECT used by get()/list() to an Incident. Exposed for unit
/// tests of the row format; nullopt if any column is missing or malformed.
[[nodiscard]] std::optional<Incident> incident_from_row(
    const std::vector<std::string>& columns, const std::vector<std::optional<std::string>>& row);

}  // namespace faulttwin::storage
