// Multi-producer / multi-consumer stress tests. Their main value is under the
// tsan preset, where ThreadSanitizer checks every interleaving these runs hit.
// Each item is unique (producer id in the high bits), so exactly-once delivery
// can be checked: every item is received by a consumer or handed back to its
// producer, never both and never neither.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <thread>
#include <vector>

#include "faulttwin/concurrency/bounded_queue.hpp"
#include "faulttwin/concurrency/queue_types.hpp"
#include "faulttwin/concurrency/unbounded_queue.hpp"

namespace faulttwin::concurrency {
namespace {

constexpr std::size_t kProducers = 4;
constexpr std::size_t kConsumers = 4;
constexpr std::uint64_t kItemsPerProducer = 20'000;

std::uint64_t make_item(std::size_t producer, std::uint64_t seq) {
    return (static_cast<std::uint64_t>(producer) << 32U) | seq;
}

struct StressOutcome {
    std::vector<std::uint64_t> received;  ///< popped by consumers
    std::vector<std::uint64_t> returned;  ///< handed back to producers
    QueueStats stats;
};

template <typename Queue>
StressOutcome run_stress(Queue& queue) {
    std::vector<std::vector<std::uint64_t>> received(kConsumers);
    std::vector<std::vector<std::uint64_t>> returned(kProducers);
    {
        std::vector<std::jthread> consumers;
        for (std::size_t c = 0; c < kConsumers; ++c) {
            consumers.emplace_back([&queue, &out = received[c]] {
                for (;;) {
                    PopResult<std::uint64_t> popped = queue.pop();
                    if (popped.status != PopStatus::kItem) {
                        return;  // closed and drained
                    }
                    out.push_back(*popped.item);  // NOLINT(bugprone-unchecked-optional-access)
                }
            });
        }
        {
            std::vector<std::jthread> producers;
            for (std::size_t p = 0; p < kProducers; ++p) {
                producers.emplace_back([&queue, p, &back = returned[p]] {
                    for (std::uint64_t i = 0; i < kItemsPerProducer; ++i) {
                        PushResult<std::uint64_t> pushed = queue.push(make_item(p, i));
                        if (pushed.item.has_value()) {
                            back.push_back(*pushed.item);
                        }
                    }
                });
            }
        }  // producers joined
        queue.close();
    }  // consumers joined

    StressOutcome outcome;
    for (const auto& part : received) {
        outcome.received.insert(outcome.received.end(), part.begin(), part.end());
    }
    for (const auto& part : returned) {
        outcome.returned.insert(outcome.returned.end(), part.begin(), part.end());
    }
    outcome.stats = queue.stats();
    return outcome;
}

/// Every produced item appears exactly once across received + returned.
void expect_exactly_once(const StressOutcome& outcome) {
    std::vector<std::uint64_t> all = outcome.received;
    all.insert(all.end(), outcome.returned.begin(), outcome.returned.end());
    std::sort(all.begin(), all.end());
    std::vector<std::uint64_t> expected;
    for (std::size_t p = 0; p < kProducers; ++p) {
        for (std::uint64_t i = 0; i < kItemsPerProducer; ++i) {
            expected.push_back(make_item(p, i));
        }
    }
    ASSERT_EQ(all.size(), expected.size());
    EXPECT_TRUE(all == expected) << "an item was lost or duplicated";
    EXPECT_TRUE(outcome.stats.invariants_hold());
    EXPECT_EQ(outcome.stats.depth, 0U);
}

TEST(QueueStressTest, BlockPolicyDeliversEveryItemExactlyOnce) {
    BoundedQueue<std::uint64_t> queue(64, OverloadPolicy::kBlock);
    const StressOutcome outcome = run_stress(queue);
    expect_exactly_once(outcome);
    EXPECT_TRUE(outcome.returned.empty());
    EXPECT_EQ(outcome.received.size(), kProducers * kItemsPerProducer);
    EXPECT_LE(outcome.stats.high_watermark, 64U);
}

TEST(QueueStressTest, RejectNewestAccountsForEveryItem) {
    BoundedQueue<std::uint64_t> queue(8, OverloadPolicy::kRejectNewest);
    const StressOutcome outcome = run_stress(queue);
    expect_exactly_once(outcome);
    EXPECT_EQ(outcome.returned.size(), outcome.stats.rejected_full);
    EXPECT_EQ(outcome.received.size(), outcome.stats.accepted);
}

TEST(QueueStressTest, DropOldestAccountsForEveryItem) {
    BoundedQueue<std::uint64_t> queue(8, OverloadPolicy::kDropOldest);
    const StressOutcome outcome = run_stress(queue);
    expect_exactly_once(outcome);
    EXPECT_EQ(outcome.returned.size(), outcome.stats.displaced_oldest);
    EXPECT_EQ(outcome.stats.rejected(), 0U);
}

TEST(QueueStressTest, UnboundedReferenceDeliversEveryItemExactlyOnce) {
    UnboundedQueue<std::uint64_t> queue;
    const StressOutcome outcome = run_stress(queue);
    expect_exactly_once(outcome);
    EXPECT_TRUE(outcome.returned.empty());
}

TEST(QueueStressTest, ConsumersSeeEachProducersItemsInOrder) {
    BoundedQueue<std::uint64_t> queue(16, OverloadPolicy::kBlock);
    std::vector<std::vector<std::uint64_t>> received(kConsumers);
    {
        std::vector<std::jthread> consumers;
        for (std::size_t c = 0; c < kConsumers; ++c) {
            consumers.emplace_back([&queue, &out = received[c]] {
                for (PopResult<std::uint64_t> r = queue.pop(); r.status == PopStatus::kItem;
                     r = queue.pop()) {
                    out.push_back(*r.item);  // NOLINT(bugprone-unchecked-optional-access)
                }
            });
        }
        {
            std::vector<std::jthread> producers;
            for (std::size_t p = 0; p < kProducers; ++p) {
                producers.emplace_back([&queue, p] {
                    for (std::uint64_t i = 0; i < kItemsPerProducer; ++i) {
                        ASSERT_TRUE(queue.push(make_item(p, i)).accepted());
                    }
                });
            }
        }
        queue.close();
    }
    // A FIFO queue hands items out in push order, so within one consumer the
    // items of any one producer must be strictly increasing.
    for (const auto& list : received) {
        std::vector<std::int64_t> last(kProducers, -1);
        for (const std::uint64_t item : list) {
            const std::size_t producer = item >> 32U;
            const auto seq = static_cast<std::int64_t>(item & 0xFFFF'FFFFU);
            ASSERT_GT(seq, last[producer]);
            last[producer] = seq;
        }
    }
}

TEST(QueueStressTest, CancellingBlockedProducersLosesNothing) {
    BoundedQueue<std::uint64_t> queue(4, OverloadPolicy::kBlock);
    std::vector<std::vector<std::uint64_t>> returned(kProducers);
    std::stop_source stop;
    {
        std::vector<std::jthread> producers;
        for (std::size_t p = 0; p < kProducers; ++p) {
            producers.emplace_back([&queue, &stop, p, &back = returned[p]] {
                for (std::uint64_t i = 0; i < 1000; ++i) {
                    PushResult<std::uint64_t> pushed =
                        queue.push(make_item(p, i), stop.get_token());
                    if (pushed.item.has_value()) {
                        back.push_back(*pushed.item);
                    }
                }
            });
        }
        // Nobody consumes: producers fill the queue and block until cancelled.
        while (queue.stats().depth < 4) {
            std::this_thread::yield();
        }
        stop.request_stop();
    }
    const QueueStats stats = queue.stats();
    std::size_t returned_total = 0;
    for (const auto& part : returned) {
        returned_total += part.size();
    }
    EXPECT_EQ(stats.accepted, 4U);
    EXPECT_EQ(returned_total, kProducers * 1000 - 4);
    EXPECT_EQ(returned_total, stats.rejected_cancelled);
    EXPECT_EQ(queue.take_all().size(), 4U);
    EXPECT_TRUE(queue.stats().invariants_hold());
}

}  // namespace
}  // namespace faulttwin::concurrency
