#pragma once

#include <atomic>

#include "faulttwin/storage/memory_incident_store.hpp"

namespace faulttwin::storage::testing {

/// A MemoryIncidentStore that can be switched to "unavailable", to test retry paths.
class FlakyStore final : public IncidentStore {
  public:
    std::atomic<bool> available{true};
    MemoryIncidentStore inner;

    StoreResult<std::uint64_t> open(const NewIncident& i) override {
        if (!available) {
            return down();
        }
        return inner.open(i);
    }
    StoreResult<Done> escalate(std::uint64_t id, const IncidentEvent& e) override {
        if (!available) {
            return down();
        }
        return inner.escalate(id, e);
    }
    StoreResult<Done> resolve(std::uint64_t id, const IncidentEvent& e, Resolution r) override {
        if (!available) {
            return down();
        }
        return inner.resolve(id, e, r);
    }
    StoreResult<std::uint64_t> close_orphans(Timestamp at) override {
        if (!available) {
            return down();
        }
        return inner.close_orphans(at);
    }
    StoreResult<Incident> acknowledge(std::uint64_t id, const Acknowledgement& a) override {
        if (!available) {
            return down();
        }
        return inner.acknowledge(id, a);
    }
    StoreResult<Incident> get(std::uint64_t id) override {
        if (!available) {
            return down();
        }
        return inner.get(id);
    }
    StoreResult<IncidentPage> list(const IncidentQuery& q) override {
        if (!available) {
            return down();
        }
        return inner.list(q);
    }
    StoreResult<StoreStats> stats() override {
        if (!available) {
            return down();
        }
        return inner.stats();
    }
    StoreResult<Done> ping() override {
        if (!available) {
            return down();
        }
        return Done{};
    }
    std::string_view backend() const noexcept override { return "flaky"; }

  private:
    static StoreError down() { return {StoreErrorKind::kUnavailable, "test: store is down"}; }
};

}  // namespace faulttwin::storage::testing
