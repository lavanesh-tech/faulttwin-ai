#include "faulttwin/ingest/pipeline.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "faulttwin/ingest/baseline.hpp"
#include "faulttwin/ingest/event_processor.hpp"
#include "faulttwin/telemetry/synthetic.hpp"
#include "ingest_test_support.hpp"

namespace faulttwin::ingest {
namespace {

using namespace std::chrono_literals;
using concurrency::OverloadPolicy;
using concurrency::PushStatus;
using testing::invalid_raw;
using testing::test_now;
using testing::valid_raw;

constexpr auto kWaitLimit = 10s;

NowFn fixed_now() {
    return [] { return test_now(); };
}

/// Thread-safe record of everything the sink saw.
class RecordingSink {
  public:
    EventSink sink() {
        return [this](std::size_t /*worker*/, const ProcessedEvent& e) {
            std::scoped_lock lock(mutex_);
            sequences_.push_back(e.event->sequence);
            dispositions_.push_back(e.disposition);
            issue_counts_.push_back(e.issues.size());
            has_sample_.push_back(e.sample != nullptr);
        };
    }
    std::vector<std::uint64_t> sequences() const {
        std::scoped_lock lock(mutex_);
        return sequences_;
    }
    std::vector<Disposition> dispositions() const {
        std::scoped_lock lock(mutex_);
        return dispositions_;
    }
    std::vector<std::size_t> issue_counts() const {
        std::scoped_lock lock(mutex_);
        return issue_counts_;
    }
    std::vector<bool> has_sample() const {
        std::scoped_lock lock(mutex_);
        return has_sample_;
    }

  private:
    mutable std::mutex mutex_;
    std::vector<std::uint64_t> sequences_;
    std::vector<Disposition> dispositions_;
    std::vector<std::size_t> issue_counts_;
    std::vector<bool> has_sample_;
};

/// A sink that blocks every call until opened or until shutdown cancels it.
class GateSink {
  public:
    EventSink sink() {
        return [this](std::size_t /*worker*/, const ProcessedEvent& e) {
            std::unique_lock lock(mutex_);
            ++entered_;
            entered_cv_.notify_all();
            gate_cv_.wait(lock, e.stop, [this] { return open_; });
        };
    }
    void open() {
        {
            std::scoped_lock lock(mutex_);
            open_ = true;
        }
        gate_cv_.notify_all();
    }
    void wait_until_entered(int n) {
        std::unique_lock lock(mutex_);
        entered_cv_.wait(lock, [&] { return entered_ >= n; });
    }

  private:
    std::mutex mutex_;
    std::condition_variable_any gate_cv_;
    std::condition_variable entered_cv_;
    int entered_{0};
    bool open_{false};
};

TEST(PipelineTest, RejectsInvalidConfiguration) {
    RecordingSink rec;
    EXPECT_THROW(IngestPipeline({.consumers = 0}, fixed_now(), rec.sink()), std::invalid_argument);
    EXPECT_THROW(IngestPipeline({.queue_capacity = 0}, fixed_now(), rec.sink()),
                 std::invalid_argument);
    EXPECT_THROW(IngestPipeline({.consumer_batch = 0}, fixed_now(), rec.sink()),
                 std::invalid_argument);
    EXPECT_THROW(IngestPipeline({}, fixed_now(), EventSink{}), std::invalid_argument);
}

TEST(PipelineTest, DeliversEveryValidEventExactlyOnce) {
    RecordingSink rec;
    IngestPipeline pipeline({.queue_capacity = 8, .consumers = 3}, fixed_now(), rec.sink());
    constexpr int kEvents = 300;
    for (int i = 0; i < kEvents; ++i) {
        // Distinct nodes so that multi-consumer reordering cannot create
        // out-of-order verdicts in this test.
        const SubmitResult r = pipeline.submit(valid_raw("node-" + std::to_string(i), 1));
        ASSERT_EQ(r.status, PushStatus::kAccepted);
        EXPECT_EQ(r.sequence, static_cast<std::uint64_t>(i));
    }
    const ShutdownReport report = pipeline.shutdown(kWaitLimit);
    EXPECT_TRUE(report.drained);
    EXPECT_TRUE(report.abandoned.empty());
    const IngestCounters& c = report.counters;
    EXPECT_EQ(c.attempted, kEvents);
    EXPECT_EQ(c.accepted, kEvents);
    EXPECT_EQ(c.processed(), kEvents);
    EXPECT_EQ(c.processing.count(Disposition::kDelivered), kEvents);
    EXPECT_EQ(c.in_queue, 0U);
    EXPECT_EQ(c.in_flight, 0U);
    EXPECT_TRUE(c.invariants_hold());

    std::vector<std::uint64_t> seen = rec.sequences();
    std::sort(seen.begin(), seen.end());
    ASSERT_EQ(seen.size(), static_cast<std::size_t>(kEvents));
    for (std::size_t i = 0; i < seen.size(); ++i) {
        EXPECT_EQ(seen[i], i);
    }
}

TEST(PipelineTest, InvalidEventsReachTheSinkWithTheirIssues) {
    RecordingSink rec;
    IngestPipeline pipeline({}, fixed_now(), rec.sink());
    ASSERT_TRUE(pipeline.submit(invalid_raw()).accepted());
    ASSERT_TRUE(pipeline.submit(valid_raw("pg-replica-2", 1)).accepted());
    const ShutdownReport report = pipeline.shutdown(kWaitLimit);
    EXPECT_EQ(report.counters.processing.count(Disposition::kRejectedInvalid), 1U);
    EXPECT_EQ(report.counters.processing.count(Disposition::kDelivered), 1U);
    EXPECT_EQ(rec.dispositions(),
              (std::vector<Disposition>{Disposition::kRejectedInvalid, Disposition::kDelivered}));
    EXPECT_EQ(rec.issue_counts(), (std::vector<std::size_t>{1, 0}));
    EXPECT_EQ(rec.has_sample(), (std::vector<bool>{false, true}));
}

struct PolicyCase {
    const char* name;
    DuplicatePolicy duplicates;
    OutOfOrderPolicy out_of_order;
    std::vector<Disposition> expected;
};

class PipelinePolicyTest : public ::testing::TestWithParam<PolicyCase> {};

TEST_P(PipelinePolicyTest, AppliesDuplicateAndOutOfOrderPolicies) {
    const PolicyCase& param = GetParam();
    RecordingSink rec;
    PipelineConfig config{.consumers = 1};  // one consumer: processing order == queue order
    config.processing.duplicates = param.duplicates;
    config.processing.out_of_order = param.out_of_order;
    IngestPipeline pipeline(config, fixed_now(), rec.sink());
    ASSERT_TRUE(pipeline.submit(valid_raw("a", 10)).accepted());  // in order
    ASSERT_TRUE(pipeline.submit(valid_raw("a", 5)).accepted());   // in order (newer)
    ASSERT_TRUE(pipeline.submit(valid_raw("a", 5)).accepted());   // duplicate
    ASSERT_TRUE(pipeline.submit(valid_raw("a", 20)).accepted());  // out of order (older)
    const ShutdownReport report = pipeline.shutdown(kWaitLimit);
    EXPECT_EQ(rec.dispositions(), param.expected);
    EXPECT_EQ(report.counters.processed(), 4U);
    EXPECT_TRUE(report.counters.invariants_hold());
}

INSTANTIATE_TEST_SUITE_P(
    Policies, PipelinePolicyTest,
    ::testing::Values(
        PolicyCase{"RejectDuplicatesAcceptLate",
                   DuplicatePolicy::kReject,
                   OutOfOrderPolicy::kAccept,
                   {Disposition::kDelivered, Disposition::kDelivered,
                    Disposition::kRejectedDuplicate, Disposition::kDeliveredOutOfOrder}},
        PolicyCase{"AcceptDuplicatesAcceptLate",
                   DuplicatePolicy::kAccept,
                   OutOfOrderPolicy::kAccept,
                   {Disposition::kDelivered, Disposition::kDelivered,
                    Disposition::kDeliveredDuplicate, Disposition::kDeliveredOutOfOrder}},
        PolicyCase{"RejectDuplicatesRejectLate",
                   DuplicatePolicy::kReject,
                   OutOfOrderPolicy::kReject,
                   {Disposition::kDelivered, Disposition::kDelivered,
                    Disposition::kRejectedDuplicate, Disposition::kRejectedOutOfOrder}}),
    [](const ::testing::TestParamInfo<PolicyCase>& param_info) {
        return std::string{param_info.param.name};
    });

TEST(PipelineTest, NodeLimitIsEnforcedAndCounted) {
    RecordingSink rec;
    PipelineConfig config{.consumers = 1};
    config.processing.ordering.max_nodes = 2;
    IngestPipeline pipeline(config, fixed_now(), rec.sink());
    for (const char* id : {"a", "b", "c"}) {
        ASSERT_TRUE(pipeline.submit(valid_raw(id, 1)).accepted());
    }
    const ShutdownReport report = pipeline.shutdown(kWaitLimit);
    EXPECT_EQ(report.counters.processing.count(Disposition::kRejectedNodeLimit), 1U);
    EXPECT_EQ(report.counters.processing.delivered(), 2U);
}

TEST(PipelineTest, SinkExceptionsAreCountedAndDoNotStopConsumers) {
    std::atomic<int> calls{0};
    IngestPipeline pipeline({.consumers = 1}, fixed_now(),
                            [&calls](std::size_t, const ProcessedEvent&) {
                                if (calls.fetch_add(1) == 0) {
                                    throw std::runtime_error("sink failure");
                                }
                            });
    ASSERT_TRUE(pipeline.submit(valid_raw("a", 2)).accepted());
    ASSERT_TRUE(pipeline.submit(valid_raw("a", 1)).accepted());
    const ShutdownReport report = pipeline.shutdown(kWaitLimit);
    EXPECT_EQ(report.counters.processing.count(Disposition::kFailed), 1U);
    EXPECT_EQ(report.counters.processing.count(Disposition::kDelivered), 1U);
    EXPECT_TRUE(report.counters.invariants_hold());
}

TEST(PipelineTest, RejectNewestHandsTheSampleBackUnderOverload) {
    GateSink gate;
    IngestPipeline pipeline({.queue_capacity = 2, .overload = OverloadPolicy::kRejectNewest},
                            fixed_now(), gate.sink());
    ASSERT_TRUE(pipeline.submit(valid_raw("a", 10)).accepted());
    gate.wait_until_entered(1);  // the consumer holds event 0; the queue is empty
    ASSERT_TRUE(pipeline.submit(valid_raw("a", 9)).accepted());
    ASSERT_TRUE(pipeline.submit(valid_raw("a", 8)).accepted());
    const SubmitResult refused = pipeline.submit(valid_raw("a", 7));
    EXPECT_EQ(refused.status, PushStatus::kRejectedFull);
    ASSERT_TRUE(refused.returned.has_value());
    EXPECT_EQ(refused.returned->collected_at, test_now() - 7s);  // NOLINT(*-unchecked-optional-*)
    gate.open();
    const ShutdownReport report = pipeline.shutdown(kWaitLimit);
    EXPECT_EQ(report.counters.rejected_full, 1U);
    EXPECT_EQ(report.counters.processed(), 3U);
    EXPECT_TRUE(report.counters.invariants_hold());
}

TEST(PipelineTest, DropOldestHandsTheEvictedSampleBack) {
    GateSink gate;
    IngestPipeline pipeline({.queue_capacity = 2, .overload = OverloadPolicy::kDropOldest},
                            fixed_now(), gate.sink());
    ASSERT_TRUE(pipeline.submit(valid_raw("a", 10)).accepted());
    gate.wait_until_entered(1);
    ASSERT_TRUE(pipeline.submit(valid_raw("a", 9)).accepted());
    ASSERT_TRUE(pipeline.submit(valid_raw("a", 8)).accepted());
    const SubmitResult displaced = pipeline.submit(valid_raw("a", 7));
    EXPECT_EQ(displaced.status, PushStatus::kAcceptedDisplacedOldest);
    ASSERT_TRUE(displaced.returned.has_value());
    EXPECT_EQ(displaced.returned->collected_at, test_now() - 9s);  // NOLINT(*-unchecked-optional-*)
    gate.open();
    const ShutdownReport report = pipeline.shutdown(kWaitLimit);
    EXPECT_EQ(report.counters.dropped_oldest, 1U);
    EXPECT_EQ(report.counters.processed(), 3U);
    EXPECT_TRUE(report.counters.invariants_hold());
}

TEST(PipelineTest, BlockingSubmitTimesOut) {
    GateSink gate;
    IngestPipeline pipeline({.queue_capacity = 1}, fixed_now(), gate.sink());
    ASSERT_TRUE(pipeline.submit(valid_raw("a", 10)).accepted());
    gate.wait_until_entered(1);
    ASSERT_TRUE(pipeline.submit(valid_raw("a", 9)).accepted());  // fills the queue
    const auto start = concurrency::QueueClock::now();
    const SubmitResult r = pipeline.submit(valid_raw("a", 8), {}, start + 30ms);
    EXPECT_EQ(r.status, PushStatus::kTimedOut);
    EXPECT_GE(concurrency::QueueClock::now() - start, 30ms);
    EXPECT_TRUE(r.returned.has_value());
    gate.open();
    EXPECT_EQ(pipeline.shutdown(kWaitLimit).counters.rejected_timeout, 1U);
}

TEST(PipelineTest, BlockingSubmitIsCancelledByStopToken) {
    GateSink gate;
    IngestPipeline pipeline({.queue_capacity = 1}, fixed_now(), gate.sink());
    ASSERT_TRUE(pipeline.submit(valid_raw("a", 10)).accepted());
    gate.wait_until_entered(1);
    ASSERT_TRUE(pipeline.submit(valid_raw("a", 9)).accepted());
    std::stop_source stop;
    auto blocked = std::async(std::launch::async,
                              [&] { return pipeline.submit(valid_raw("a", 8), stop.get_token()); });
    stop.request_stop();
    ASSERT_EQ(blocked.wait_for(kWaitLimit), std::future_status::ready);
    const SubmitResult r = blocked.get();
    EXPECT_EQ(r.status, PushStatus::kCancelled);
    EXPECT_TRUE(r.returned.has_value());
    gate.open();
    EXPECT_EQ(pipeline.shutdown(kWaitLimit).counters.rejected_cancelled, 1U);
}

TEST(PipelineTest, ShutdownWakesBlockedProducers) {
    GateSink gate;
    IngestPipeline pipeline({.queue_capacity = 1}, fixed_now(), gate.sink());
    ASSERT_TRUE(pipeline.submit(valid_raw("a", 10)).accepted());
    gate.wait_until_entered(1);
    ASSERT_TRUE(pipeline.submit(valid_raw("a", 9)).accepted());
    auto blocked =
        std::async(std::launch::async, [&] { return pipeline.submit(valid_raw("a", 8)); });
    // The blocked producer may not have started waiting yet; either way it ends kClosed.
    gate.open();
    const ShutdownReport report = pipeline.shutdown(kWaitLimit);
    ASSERT_EQ(blocked.wait_for(kWaitLimit), std::future_status::ready);
    const SubmitResult r = blocked.get();
    // Depending on timing the third sample was accepted before close or refused by it.
    if (r.accepted()) {
        EXPECT_EQ(pipeline.counters().processed(), 3U);
    } else {
        EXPECT_EQ(r.status, PushStatus::kClosed);
        EXPECT_TRUE(r.returned.has_value());
    }
    EXPECT_TRUE(pipeline.counters().invariants_hold());
    static_cast<void>(report);
}

TEST(PipelineTest, SubmitAfterShutdownIsRefusedWithTheSample) {
    RecordingSink rec;
    IngestPipeline pipeline({}, fixed_now(), rec.sink());
    static_cast<void>(pipeline.shutdown(kWaitLimit));
    const SubmitResult r = pipeline.submit(valid_raw("a", 1));
    EXPECT_EQ(r.status, PushStatus::kClosed);
    EXPECT_TRUE(r.returned.has_value());
    EXPECT_EQ(pipeline.counters().rejected_closed, 1U);
}

TEST(PipelineTest, ShutdownDeadlineAbandonsAndReturnsUnprocessedEvents) {
    GateSink gate;  // never opened: the consumer is released only by cancellation
    IngestPipeline pipeline({.queue_capacity = 16, .consumers = 1}, fixed_now(), gate.sink());
    constexpr int kEvents = 10;
    for (int i = 0; i < kEvents; ++i) {
        ASSERT_TRUE(pipeline.submit(valid_raw("a", 100 - i)).accepted());
    }
    gate.wait_until_entered(1);
    const ShutdownReport report = pipeline.shutdown(20ms);
    EXPECT_FALSE(report.drained);
    EXPECT_GE(report.elapsed, 20ms);
    const IngestCounters& c = report.counters;
    // One event was inside the sink when cancellation arrived; it completes and
    // counts as processed. The rest are handed back, not destroyed.
    EXPECT_EQ(c.processed(), 1U);
    EXPECT_EQ(report.abandoned.size(), static_cast<std::size_t>(kEvents - 1));
    EXPECT_EQ(c.abandoned_at_shutdown, static_cast<std::uint64_t>(kEvents - 1));
    EXPECT_EQ(c.in_queue + c.in_flight, 0U);
    EXPECT_TRUE(c.invariants_hold());
    // Abandoned samples come back in queue order.
    EXPECT_EQ(report.abandoned.front().collected_at, test_now() - 99s);
}

TEST(PipelineTest, CancelledConsumerHandsBackTheRestOfItsBatch) {
    GateSink gate;
    // Large batch: the consumer may hold many events when it is cancelled.
    IngestPipeline pipeline({.queue_capacity = 64, .consumers = 1, .consumer_batch = 64},
                            fixed_now(), gate.sink());
    constexpr int kEvents = 40;
    for (int i = 0; i < kEvents; ++i) {
        ASSERT_TRUE(pipeline.submit(valid_raw("a", 1000 - i)).accepted());
    }
    gate.wait_until_entered(1);
    const ShutdownReport report = pipeline.shutdown(10ms);
    EXPECT_EQ(report.counters.processed(), 1U);
    ASSERT_EQ(report.abandoned.size(), static_cast<std::size_t>(kEvents - 1));
    // Batch remainder first, then the queue: original order is preserved.
    for (std::size_t i = 0; i < report.abandoned.size(); ++i) {
        EXPECT_EQ(report.abandoned[i].collected_at,
                  test_now() - std::chrono::seconds{999 - static_cast<int>(i)});
    }
    EXPECT_TRUE(report.counters.invariants_hold());
}

TEST(PipelineTest, UnbatchedConsumersDeliverEverything) {
    RecordingSink rec;
    IngestPipeline pipeline({.queue_capacity = 4, .consumers = 2, .consumer_batch = 1}, fixed_now(),
                            rec.sink());
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(pipeline.submit(valid_raw("n" + std::to_string(i), 1)).accepted());
    }
    EXPECT_EQ(pipeline.shutdown(kWaitLimit).counters.processed(), 100U);
}

TEST(PipelineTest, ShutdownIsIdempotent) {
    RecordingSink rec;
    IngestPipeline pipeline({}, fixed_now(), rec.sink());
    ASSERT_TRUE(pipeline.submit(valid_raw("a", 1)).accepted());
    const ShutdownReport first = pipeline.shutdown(kWaitLimit);
    const ShutdownReport second = pipeline.shutdown(kWaitLimit);
    EXPECT_FALSE(first.already_shut_down);
    EXPECT_TRUE(second.already_shut_down);
    EXPECT_EQ(second.counters.processed(), 1U);
}

TEST(PipelineTest, DestructorDrainsWithoutExplicitShutdown) {
    RecordingSink rec;
    {
        IngestPipeline pipeline({.consumers = 2}, fixed_now(), rec.sink());
        for (int i = 0; i < 50; ++i) {
            ASSERT_TRUE(pipeline.submit(valid_raw("n" + std::to_string(i), 1)).accepted());
        }
    }
    EXPECT_EQ(rec.sequences().size(), 50U);
}

TEST(PipelineTest, ManyProducersAndConsumersKeepTheInvariants) {
    std::atomic<std::uint64_t> delivered{0};
    PipelineConfig config{.queue_capacity = 32, .consumers = 4};
    IngestPipeline pipeline(config, fixed_now(), [&](std::size_t, const ProcessedEvent& e) {
        if (is_delivered(e.disposition)) {
            delivered.fetch_add(1, std::memory_order_relaxed);
        }
    });
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 5'000;
    {
        std::vector<std::jthread> producers;
        for (int p = 0; p < kProducers; ++p) {
            producers.emplace_back([&pipeline, p] {
                const std::string id = "node-" + std::to_string(p);
                for (int i = 0; i < kPerProducer; ++i) {
                    // Every 100th event is invalid; every 50th repeats its predecessor.
                    telemetry::RawTelemetrySample raw = valid_raw(id, 100'000 - i);
                    if (i % 100 == 99) {
                        raw = invalid_raw();
                    } else if (i % 50 == 24) {
                        raw = valid_raw(id, 100'000 - (i - 1));
                    }
                    ASSERT_TRUE(pipeline.submit(std::move(raw)).accepted());
                }
            });
        }
    }
    const ShutdownReport report = pipeline.shutdown(kWaitLimit);
    const IngestCounters& c = report.counters;
    EXPECT_TRUE(report.drained);
    EXPECT_EQ(c.attempted, static_cast<std::uint64_t>(kProducers * kPerProducer));
    EXPECT_EQ(c.processed(), c.accepted);
    EXPECT_EQ(c.processing.delivered(), delivered.load());
    EXPECT_EQ(c.processing.count(Disposition::kRejectedInvalid),
              static_cast<std::uint64_t>(kProducers * (kPerProducer / 100)));
    EXPECT_TRUE(c.invariants_hold());
}

TEST(BaselineTest, MatchesThePipelineOnTheSameInput) {
    telemetry::SyntheticConfig synth;
    synth.node_count = 4;
    synth.samples_per_node = 250;
    synth.invalid_fraction = 0.1;
    synth.seed = 99;
    const auto samples = telemetry::generate_synthetic_samples(synth);
    const telemetry::Timestamp now = telemetry::synthetic_reference_now(synth);
    std::vector<telemetry::RawTelemetrySample> raws;
    for (const auto& s : samples) {
        raws.push_back(s.raw);
    }

    EventProcessor processor(
        {}, 1, [now] { return now; }, [](std::size_t, const ProcessedEvent&) {});
    const ProcessingCounters baseline = run_single_threaded_baseline(raws, processor);

    IngestPipeline pipeline(
        {.consumers = 1}, [now] { return now; }, [](std::size_t, const ProcessedEvent&) {});
    for (auto& raw : raws) {
        ASSERT_TRUE(pipeline.submit(std::move(raw)).accepted());
    }
    const ShutdownReport report = pipeline.shutdown(kWaitLimit);
    EXPECT_EQ(report.counters.processing.by_disposition, baseline.by_disposition);
    EXPECT_EQ(baseline.processed(), samples.size());
    EXPECT_GT(baseline.count(Disposition::kRejectedInvalid), 0U);
}

}  // namespace
}  // namespace faulttwin::ingest
