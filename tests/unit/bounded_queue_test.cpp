#include "faulttwin/concurrency/bounded_queue.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <thread>

#include "faulttwin/concurrency/unbounded_queue.hpp"

namespace faulttwin::concurrency {
namespace {

using namespace std::chrono_literals;

static_assert(QueueLike<BoundedQueue<int>, int>);
static_assert(QueueLike<UnboundedQueue<int>, int>);
static_assert(QueueLike<BoundedQueue<std::unique_ptr<int>>, std::unique_ptr<int>>);

// Generous upper bound for "should wake promptly" checks; sanitizer builds and
// loaded CI machines are slow, so tests only assert lower bounds on timing.
constexpr auto kWaitLimit = 10s;

TEST(BoundedQueueTest, ZeroCapacityIsAConfigurationError) {
    EXPECT_THROW(BoundedQueue<int>(0, OverloadPolicy::kBlock), std::invalid_argument);
}

TEST(BoundedQueueTest, IsFifo) {
    BoundedQueue<int> queue(4, OverloadPolicy::kBlock);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(queue.push(i).status, PushStatus::kAccepted);
    }
    for (int i = 0; i < 4; ++i) {
        const PopResult<int> popped = queue.try_pop();
        ASSERT_EQ(popped.status, PopStatus::kItem);
        EXPECT_EQ(popped.item, i);
    }
    EXPECT_EQ(queue.try_pop().status, PopStatus::kTimedOut);
}

TEST(BoundedQueueTest, WrapsAroundTheRingBuffer) {
    BoundedQueue<int> queue(3, OverloadPolicy::kBlock);
    for (int round = 0; round < 10; ++round) {
        ASSERT_TRUE(queue.try_push(round).accepted());
        ASSERT_TRUE(queue.try_push(round + 100).accepted());
        EXPECT_EQ(queue.try_pop().item, round);
        EXPECT_EQ(queue.try_pop().item, round + 100);
    }
    EXPECT_TRUE(queue.stats().invariants_hold());
}

TEST(BoundedQueueTest, TryPushOnFullBlockingQueueRejectsAndReturnsTheItem) {
    BoundedQueue<int> queue(1, OverloadPolicy::kBlock);
    ASSERT_TRUE(queue.try_push(1).accepted());
    const PushResult<int> result = queue.try_push(2);
    EXPECT_EQ(result.status, PushStatus::kRejectedFull);
    EXPECT_EQ(result.item, 2);
    const QueueStats stats = queue.stats();
    EXPECT_EQ(stats.rejected_full, 1U);
    EXPECT_TRUE(stats.invariants_hold());
}

TEST(BoundedQueueTest, RejectNewestNeverWaitsAndReturnsTheNewItem) {
    BoundedQueue<int> queue(2, OverloadPolicy::kRejectNewest);
    ASSERT_TRUE(queue.push(1).accepted());
    ASSERT_TRUE(queue.push(2).accepted());
    // No deadline and no stop token: a blocking implementation would hang here.
    const PushResult<int> result = queue.push(3);
    EXPECT_EQ(result.status, PushStatus::kRejectedFull);
    EXPECT_EQ(result.item, 3);
    EXPECT_EQ(queue.try_pop().item, 1);
    EXPECT_EQ(queue.try_pop().item, 2);
    const QueueStats stats = queue.stats();
    EXPECT_EQ(stats.attempted, 3U);
    EXPECT_EQ(stats.accepted, 2U);
    EXPECT_EQ(stats.rejected_full, 1U);
    EXPECT_TRUE(stats.invariants_hold());
}

TEST(BoundedQueueTest, DropOldestEvictsAndReturnsTheOldestItem) {
    BoundedQueue<int> queue(2, OverloadPolicy::kDropOldest);
    ASSERT_TRUE(queue.push(1).accepted());
    ASSERT_TRUE(queue.push(2).accepted());
    const PushResult<int> result = queue.push(3);
    EXPECT_EQ(result.status, PushStatus::kAcceptedDisplacedOldest);
    EXPECT_TRUE(result.accepted());
    EXPECT_EQ(result.item, 1);  // handed back, not destroyed
    EXPECT_EQ(queue.try_pop().item, 2);
    EXPECT_EQ(queue.try_pop().item, 3);
    const QueueStats stats = queue.stats();
    EXPECT_EQ(stats.accepted, 3U);
    EXPECT_EQ(stats.displaced_oldest, 1U);
    EXPECT_EQ(stats.removed, 2U);
    EXPECT_TRUE(stats.invariants_hold());
}

TEST(BoundedQueueTest, BlockingPushTimesOutAndReturnsTheItem) {
    BoundedQueue<int> queue(1, OverloadPolicy::kBlock);
    ASSERT_TRUE(queue.push(1).accepted());
    constexpr auto kTimeout = 30ms;
    const auto start = QueueClock::now();
    const PushResult<int> result = queue.push(2, {}, start + kTimeout);
    const auto waited = QueueClock::now() - start;
    EXPECT_EQ(result.status, PushStatus::kTimedOut);
    EXPECT_EQ(result.item, 2);
    EXPECT_GE(waited, kTimeout);
    EXPECT_EQ(queue.stats().rejected_timeout, 1U);
}

TEST(BoundedQueueTest, BlockingPushIsCancelledByStopToken) {
    BoundedQueue<int> queue(1, OverloadPolicy::kBlock);
    ASSERT_TRUE(queue.push(1).accepted());
    std::stop_source stop;
    auto pushed = std::async(std::launch::async, [&] { return queue.push(2, stop.get_token()); });
    // Whether the request lands before or during the wait, the result is kCancelled.
    stop.request_stop();
    ASSERT_EQ(pushed.wait_for(kWaitLimit), std::future_status::ready);
    const PushResult<int> result = pushed.get();
    EXPECT_EQ(result.status, PushStatus::kCancelled);
    EXPECT_EQ(result.item, 2);
    EXPECT_EQ(queue.stats().rejected_cancelled, 1U);
}

TEST(BoundedQueueTest, BlockingPushProceedsWhenSpaceIsFreed) {
    BoundedQueue<int> queue(1, OverloadPolicy::kBlock);
    ASSERT_TRUE(queue.push(1).accepted());
    auto pushed = std::async(std::launch::async, [&] { return queue.push(2); });
    EXPECT_EQ(queue.pop().item, 1);
    ASSERT_EQ(pushed.wait_for(kWaitLimit), std::future_status::ready);
    EXPECT_EQ(pushed.get().status, PushStatus::kAccepted);
    EXPECT_EQ(queue.pop().item, 2);
}

TEST(BoundedQueueTest, CloseWakesBlockedProducerWithItsItem) {
    BoundedQueue<int> queue(1, OverloadPolicy::kBlock);
    ASSERT_TRUE(queue.push(1).accepted());
    auto pushed = std::async(std::launch::async, [&] { return queue.push(2); });
    queue.close();
    ASSERT_EQ(pushed.wait_for(kWaitLimit), std::future_status::ready);
    const PushResult<int> result = pushed.get();
    EXPECT_EQ(result.status, PushStatus::kClosed);
    EXPECT_EQ(result.item, 2);
}

TEST(BoundedQueueTest, ClosedQueueRefusesPushesButDrainsForConsumers) {
    BoundedQueue<int> queue(4, OverloadPolicy::kBlock);
    ASSERT_TRUE(queue.push(1).accepted());
    ASSERT_TRUE(queue.push(2).accepted());
    queue.close();
    queue.close();  // idempotent
    EXPECT_TRUE(queue.is_closed());
    const PushResult<int> refused = queue.push(3);
    EXPECT_EQ(refused.status, PushStatus::kClosed);
    EXPECT_EQ(refused.item, 3);
    EXPECT_EQ(queue.pop().item, 1);
    EXPECT_EQ(queue.pop().item, 2);
    EXPECT_EQ(queue.pop().status, PopStatus::kClosed);
    const QueueStats stats = queue.stats();
    EXPECT_EQ(stats.rejected_closed, 1U);
    EXPECT_TRUE(stats.invariants_hold());
}

TEST(BoundedQueueTest, CloseWakesBlockedConsumer) {
    BoundedQueue<int> queue(1, OverloadPolicy::kBlock);
    auto popped = std::async(std::launch::async, [&] { return queue.pop(); });
    queue.close();
    ASSERT_EQ(popped.wait_for(kWaitLimit), std::future_status::ready);
    EXPECT_EQ(popped.get().status, PopStatus::kClosed);
}

TEST(BoundedQueueTest, PopTimesOutOnEmptyQueue) {
    BoundedQueue<int> queue(1, OverloadPolicy::kBlock);
    constexpr auto kTimeout = 30ms;
    const auto start = QueueClock::now();
    EXPECT_EQ(queue.pop({}, start + kTimeout).status, PopStatus::kTimedOut);
    EXPECT_GE(QueueClock::now() - start, kTimeout);
}

TEST(BoundedQueueTest, PopIsCancelledByStopToken) {
    BoundedQueue<int> queue(1, OverloadPolicy::kBlock);
    std::stop_source stop;
    auto popped = std::async(std::launch::async, [&] { return queue.pop(stop.get_token()); });
    stop.request_stop();
    ASSERT_EQ(popped.wait_for(kWaitLimit), std::future_status::ready);
    EXPECT_EQ(popped.get().status, PopStatus::kCancelled);
}

TEST(BoundedQueueTest, PopReturnsAvailableItemEvenIfStopWasRequested) {
    BoundedQueue<int> queue(1, OverloadPolicy::kBlock);
    ASSERT_TRUE(queue.push(7).accepted());
    std::stop_source stop;
    stop.request_stop();
    const PopResult<int> popped = queue.pop(stop.get_token());
    EXPECT_EQ(popped.status, PopStatus::kItem);
    EXPECT_EQ(popped.item, 7);
}

TEST(BoundedQueueTest, TakeAllEmptiesTheQueueAndCountsRemoval) {
    BoundedQueue<int> queue(4, OverloadPolicy::kBlock);
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(queue.push(i).accepted());
    }
    EXPECT_EQ(queue.take_all(), (std::vector<int>{0, 1, 2}));
    const QueueStats stats = queue.stats();
    EXPECT_EQ(stats.depth, 0U);
    EXPECT_EQ(stats.removed, 3U);
    EXPECT_EQ(stats.high_watermark, 3U);
    EXPECT_TRUE(stats.invariants_hold());
}

TEST(BoundedQueueTest, PopBatchTakesUpToMaxItemsInOrder) {
    BoundedQueue<int> queue(8, OverloadPolicy::kBlock);
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(queue.push(i).accepted());
    }
    std::vector<int> out{-1};
    EXPECT_EQ(queue.pop_batch(out, 3), PopStatus::kItem);
    EXPECT_EQ(out, (std::vector<int>{-1, 0, 1, 2}));  // appended, not cleared
    out.clear();
    EXPECT_EQ(queue.pop_batch(out, 0), PopStatus::kItem);  // 0 is treated as 1
    EXPECT_EQ(out, (std::vector<int>{3}));
    out.clear();
    EXPECT_EQ(queue.pop_batch(out, 100), PopStatus::kItem);
    EXPECT_EQ(out, (std::vector<int>{4}));
    out.clear();
    EXPECT_EQ(queue.pop_batch(out, 4, {}, QueueClock::now() + 5ms), PopStatus::kTimedOut);
    queue.close();
    EXPECT_EQ(queue.pop_batch(out, 4), PopStatus::kClosed);
    EXPECT_TRUE(out.empty());
    EXPECT_TRUE(queue.stats().invariants_hold());
}

TEST(BoundedQueueTest, PopBatchWakesBlockedProducers) {
    BoundedQueue<int> queue(2, OverloadPolicy::kBlock);
    ASSERT_TRUE(queue.push(1).accepted());
    ASSERT_TRUE(queue.push(2).accepted());
    auto a = std::async(std::launch::async, [&] { return queue.push(3).status; });
    auto b = std::async(std::launch::async, [&] { return queue.push(4).status; });
    std::vector<int> out;
    while (out.size() < 4) {
        ASSERT_EQ(queue.pop_batch(out, 2, {}, QueueClock::now() + kWaitLimit), PopStatus::kItem);
    }
    EXPECT_EQ(a.get(), PushStatus::kAccepted);
    EXPECT_EQ(b.get(), PushStatus::kAccepted);
}

TEST(BoundedQueueTest, SupportsMoveOnlyTypes) {
    BoundedQueue<std::unique_ptr<int>> queue(1, OverloadPolicy::kRejectNewest);
    ASSERT_TRUE(queue.push(std::make_unique<int>(1)).accepted());
    PushResult<std::unique_ptr<int>> refused = queue.push(std::make_unique<int>(2));
    ASSERT_EQ(refused.status, PushStatus::kRejectedFull);
    ASSERT_TRUE(refused.item.has_value());
    ASSERT_NE(*refused.item, nullptr);
    EXPECT_EQ(**refused.item, 2);
    PopResult<std::unique_ptr<int>> popped = queue.pop();
    ASSERT_TRUE(popped.item.has_value());
    EXPECT_EQ(**popped.item, 1);
}

TEST(BoundedQueueTest, PolicyNamesRoundTrip) {
    for (const OverloadPolicy policy :
         {OverloadPolicy::kBlock, OverloadPolicy::kRejectNewest, OverloadPolicy::kDropOldest}) {
        EXPECT_EQ(parse_overload_policy(to_string(policy)), policy);
    }
    EXPECT_EQ(parse_overload_policy("drop-newest"), std::nullopt);
}

TEST(UnboundedQueueTest, AcceptsEverythingUntilClosed) {
    UnboundedQueue<int> queue;
    for (int i = 0; i < 1000; ++i) {
        ASSERT_EQ(queue.push(i).status, PushStatus::kAccepted);
    }
    EXPECT_EQ(queue.stats().high_watermark, 1000U);
    queue.close();
    EXPECT_EQ(queue.push(1).status, PushStatus::kClosed);
    EXPECT_EQ(queue.pop().item, 0);
    EXPECT_EQ(queue.take_all().size(), 999U);
    EXPECT_EQ(queue.pop().status, PopStatus::kClosed);
    EXPECT_TRUE(queue.stats().invariants_hold());
}

TEST(UnboundedQueueTest, PopTimesOutAndCancels) {
    UnboundedQueue<int> queue;
    EXPECT_EQ(queue.pop({}, QueueClock::now() + 10ms).status, PopStatus::kTimedOut);
    std::stop_source stop;
    stop.request_stop();
    EXPECT_EQ(queue.pop(stop.get_token()).status, PopStatus::kCancelled);
}

}  // namespace
}  // namespace faulttwin::concurrency
