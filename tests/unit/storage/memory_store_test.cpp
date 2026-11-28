#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "faulttwin/storage/memory_incident_store.hpp"
#include "incident_store_contract.hpp"

namespace faulttwin::storage::contract {
INSTANTIATE_TEST_SUITE_P(Memory, IncidentStoreContract, ::testing::Values(StoreFactory{[] {
                             return std::make_unique<MemoryIncidentStore>();
                         }}));
}  // namespace faulttwin::storage::contract

namespace faulttwin::storage {
namespace {

using namespace std::chrono_literals;
using contract::event;
using contract::kT0;
using contract::new_incident;

TEST(MemoryIncidentStore, EvictsTheOldestResolvedIncidentWhenFull) {
    MemoryIncidentStore store(2);
    const auto a = std::get<std::uint64_t>(store.open(new_incident("r", "a")));
    const auto b = std::get<std::uint64_t>(store.open(new_incident("r", "b")));
    ASSERT_TRUE(std::holds_alternative<Done>(store.resolve(
        a, event(DetectionKind::kResolved, Severity::kWarning, kT0 + 1s), Resolution::kRecovered)));
    const auto c = std::get<std::uint64_t>(store.open(new_incident("r", "c")));
    EXPECT_EQ(store.evicted(), 1U);
    EXPECT_TRUE(std::holds_alternative<StoreError>(store.get(a)));
    EXPECT_TRUE(std::holds_alternative<Incident>(store.get(b)));
    EXPECT_TRUE(std::holds_alternative<Incident>(store.get(c)));
}

TEST(MemoryIncidentStore, RefusesRatherThanDropsOpenIncidentsWhenFull) {
    MemoryIncidentStore store(2);
    (void)store.open(new_incident("r", "a"));
    (void)store.open(new_incident("r", "b"));
    auto result = store.open(new_incident("r", "c"));
    ASSERT_TRUE(std::holds_alternative<StoreError>(result));
    EXPECT_EQ(std::get<StoreError>(result).kind, StoreErrorKind::kCapacity);
    EXPECT_EQ(store.evicted(), 0U);
}

TEST(MemoryIncidentStore, ConcurrentWritersAndReaders) {
    MemoryIncidentStore store;
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&store, t] {
                for (int i = 0; i < 200; ++i) {
                    const auto id = std::get<std::uint64_t>(
                        store.open(new_incident("r" + std::to_string(t), "n" + std::to_string(i))));
                    (void)store.resolve(id,
                                        event(DetectionKind::kResolved, Severity::kWarning, kT0),
                                        Resolution::kRecovered);
                }
            });
        }
        threads.emplace_back([&store] {
            for (int i = 0; i < 200; ++i) {
                (void)store.list(IncidentQuery{});
                (void)store.stats();
            }
        });
    }
    const auto stats = std::get<StoreStats>(store.stats());
    EXPECT_EQ(stats.open, 0U);
    EXPECT_EQ(stats.resolved, 800U);
}

TEST(IncidentModel, EvidenceJsonRoundTrips) {
    const std::vector<Evidence> evidence{{"a.last", 1.5}, {"b.mean", std::nullopt}};
    const auto json = evidence_json(evidence);
    EXPECT_EQ(json, R"({"a.last":1.5,"b.mean":null})");
    const auto back = evidence_from_json(json);
    ASSERT_TRUE(back.has_value());
    ASSERT_EQ(back->size(), 2U);
    EXPECT_EQ((*back)[0].value, 1.5);
    EXPECT_FALSE((*back)[1].value.has_value());
    EXPECT_FALSE(evidence_from_json(R"({"a":"text"})").has_value());
    EXPECT_FALSE(evidence_from_json("[1]").has_value());
}

}  // namespace
}  // namespace faulttwin::storage
