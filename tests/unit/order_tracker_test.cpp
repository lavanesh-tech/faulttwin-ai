#include "faulttwin/ingest/order_tracker.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "ingest_test_support.hpp"

namespace faulttwin::ingest {
namespace {

using std::chrono::seconds;
using testing::node;
using testing::test_now;

TEST(OrderTrackerTest, RejectsZeroConfiguration) {
    EXPECT_THROW(NodeOrderTracker({.recent_window = 0, .max_nodes = 1, .shards = 1}),
                 std::invalid_argument);
    EXPECT_THROW(NodeOrderTracker({.recent_window = 1, .max_nodes = 0, .shards = 1}),
                 std::invalid_argument);
    EXPECT_THROW(NodeOrderTracker({.recent_window = 1, .max_nodes = 1, .shards = 0}),
                 std::invalid_argument);
}

TEST(OrderTrackerTest, ClassifiesInOrderDuplicateAndOutOfOrder) {
    NodeOrderTracker tracker;
    const auto a = node("pg-replica-1");
    EXPECT_EQ(tracker.observe(a, test_now()), OrderVerdict::kInOrder);
    EXPECT_EQ(tracker.observe(a, test_now() + seconds{1}), OrderVerdict::kInOrder);
    EXPECT_EQ(tracker.observe(a, test_now() + seconds{1}), OrderVerdict::kDuplicate);
    EXPECT_EQ(tracker.observe(a, test_now()), OrderVerdict::kDuplicate);
    EXPECT_EQ(tracker.observe(a, test_now() - seconds{5}), OrderVerdict::kOutOfOrder);
    // A late sample, once seen, is remembered: repeating it is a duplicate.
    EXPECT_EQ(tracker.observe(a, test_now() - seconds{5}), OrderVerdict::kDuplicate);
    EXPECT_EQ(tracker.tracked_nodes(), 1U);
}

TEST(OrderTrackerTest, NodesAreIndependent) {
    NodeOrderTracker tracker;
    EXPECT_EQ(tracker.observe(node("a"), test_now()), OrderVerdict::kInOrder);
    EXPECT_EQ(tracker.observe(node("b"), test_now()), OrderVerdict::kInOrder);
    EXPECT_EQ(tracker.observe(node("b"), test_now() - seconds{1}), OrderVerdict::kOutOfOrder);
    EXPECT_EQ(tracker.observe(node("a"), test_now() + seconds{1}), OrderVerdict::kInOrder);
}

TEST(OrderTrackerTest, DuplicateOlderThanWindowIsReportedAsOutOfOrder) {
    NodeOrderTracker tracker({.recent_window = 2, .max_nodes = 8, .shards = 1});
    const auto a = node("a");
    ASSERT_EQ(tracker.observe(a, test_now()), OrderVerdict::kInOrder);
    ASSERT_EQ(tracker.observe(a, test_now() + seconds{1}), OrderVerdict::kInOrder);
    ASSERT_EQ(tracker.observe(a, test_now() + seconds{2}), OrderVerdict::kInOrder);
    // test_now() has left the 2-entry window: documented limitation.
    EXPECT_EQ(tracker.observe(a, test_now()), OrderVerdict::kOutOfOrder);
}

TEST(OrderTrackerTest, NewestIsAlwaysADuplicateEvenAfterEviction) {
    NodeOrderTracker tracker({.recent_window = 1, .max_nodes = 8, .shards = 1});
    const auto a = node("a");
    ASSERT_EQ(tracker.observe(a, test_now()), OrderVerdict::kInOrder);
    ASSERT_EQ(tracker.observe(a, test_now() - seconds{3}), OrderVerdict::kOutOfOrder);
    EXPECT_EQ(tracker.observe(a, test_now()), OrderVerdict::kDuplicate);
}

TEST(OrderTrackerTest, EnforcesTheNodeLimit) {
    NodeOrderTracker tracker({.recent_window = 4, .max_nodes = 2, .shards = 4});
    EXPECT_EQ(tracker.observe(node("a"), test_now()), OrderVerdict::kInOrder);
    EXPECT_EQ(tracker.observe(node("b"), test_now()), OrderVerdict::kInOrder);
    EXPECT_EQ(tracker.observe(node("c"), test_now()), OrderVerdict::kNodeLimitExceeded);
    // Known nodes keep working at the limit.
    EXPECT_EQ(tracker.observe(node("a"), test_now() + seconds{1}), OrderVerdict::kInOrder);
    EXPECT_EQ(tracker.tracked_nodes(), 2U);
}

TEST(OrderTrackerTest, ConcurrentObserversNeverExceedTheNodeLimit) {
    constexpr std::size_t kLimit = 50;
    NodeOrderTracker tracker({.recent_window = 4, .max_nodes = kLimit, .shards = 8});
    std::atomic<std::size_t> refused{0};
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&tracker, &refused, t] {
                for (int i = 0; i < 100; ++i) {
                    const auto id = node("n-" + std::to_string(t) + "-" + std::to_string(i));
                    if (tracker.observe(id, test_now()) == OrderVerdict::kNodeLimitExceeded) {
                        refused.fetch_add(1);
                    }
                }
            });
        }
    }
    EXPECT_EQ(tracker.tracked_nodes(), kLimit);
    EXPECT_EQ(refused.load(), 400U - kLimit);
}

TEST(OrderTrackerTest, ConcurrentObserversOnOneNodeSeeEachTimestampOnce) {
    NodeOrderTracker tracker({.recent_window = 4096, .max_nodes = 4, .shards = 1});
    const auto a = node("a");
    std::atomic<int> first_sightings{0};
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < 1000; ++i) {
                    if (tracker.observe(a, test_now() + seconds{i}) != OrderVerdict::kDuplicate) {
                        first_sightings.fetch_add(1);
                    }
                }
            });
        }
    }
    // Four threads offer the same 1000 timestamps: exactly one sighting each.
    EXPECT_EQ(first_sightings.load(), 1000);
}

}  // namespace
}  // namespace faulttwin::ingest
