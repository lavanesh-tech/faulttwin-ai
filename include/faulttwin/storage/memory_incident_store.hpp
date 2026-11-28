#pragma once

#include <cstdint>
#include <map>
#include <shared_mutex>
#include <string>
#include <utility>

#include "faulttwin/storage/incident_store.hpp"

namespace faulttwin::storage {

/// In-process store, for tests and for running without a meta-db. Bounded: once `capacity`
/// incidents are held, opening a new one first evicts the oldest *resolved* incident (counted
/// in evicted()); if every held incident is open, open() fails with kCapacity rather than
/// dropping live state. Thread-safe with a reader-writer lock: API reads proceed in parallel,
/// writes are exclusive. Contents are lost when the process exits.
class MemoryIncidentStore final : public IncidentStore {
  public:
    explicit MemoryIncidentStore(std::size_t capacity = 10'000);

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
    [[nodiscard]] StoreResult<Done> ping() override { return Done{}; }
    [[nodiscard]] std::string_view backend() const noexcept override { return "memory"; }

    [[nodiscard]] std::uint64_t evicted() const;

  private:
    std::size_t capacity_;
    mutable std::shared_mutex mutex_;
    std::map<std::uint64_t, Incident> incidents_;  ///< ordered by id = creation order
    std::map<std::pair<std::string, std::string>, std::uint64_t> open_by_key_;
    std::uint64_t next_id_{1};
    std::uint64_t evicted_{0};
};

/// True when `incident` satisfies every filter of `query` except paging.
[[nodiscard]] bool matches(const Incident& incident, const IncidentQuery& query);

}  // namespace faulttwin::storage
