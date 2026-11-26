#include "faulttwin/ingest/pipeline.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace faulttwin::ingest {

namespace {

constexpr std::chrono::nanoseconds kMaxDrainTimeout = std::chrono::hours{24};

std::size_t checked_consumers(const PipelineConfig& config) {
    if (config.consumers == 0) {
        throw std::invalid_argument("IngestPipeline needs at least one consumer");
    }
    if (config.consumer_batch == 0) {
        throw std::invalid_argument("IngestPipeline consumer_batch must be at least 1");
    }
    return config.consumers;
}

SubmitResult to_submit_result(concurrency::PushResult<IngestEvent> pushed, std::uint64_t sequence) {
    SubmitResult result{.status = pushed.status, .returned = std::nullopt, .sequence = sequence};
    if (pushed.item.has_value()) {
        result.returned = std::move(pushed.item->raw);
    }
    return result;
}

}  // namespace

IngestPipeline::IngestPipeline(const PipelineConfig& config, NowFn now, EventSink sink)
    : config_(config),
      processor_(config.processing, checked_consumers(config), std::move(now), std::move(sink)),
      queue_(config.queue_capacity, config.overload) {
    consumers_.reserve(config_.consumers);
    try {
        for (std::size_t worker = 0; worker < config_.consumers; ++worker) {
            consumers_.emplace_back(
                [this, worker](const std::stop_token& stop) { consume(stop, worker); });
        }
    } catch (...) {
        // Thread creation failed: stop the consumers already started, then
        // rethrow. Clearing the vector joins them.
        queue_.close();
        for (std::jthread& consumer : consumers_) {
            consumer.request_stop();
        }
        consumers_.clear();
        throw;
    }
}

IngestPipeline::~IngestPipeline() {
    // A destructor must not throw; shutdown() only throws on std::system_error
    // from the threading primitives, which would leave no safe recovery anyway.
    try {
        static_cast<void>(shutdown(config_.destructor_drain_timeout));
    } catch (...) {  // NOLINT(bugprone-empty-catch): nothing useful can be done here
    }
}

void IngestPipeline::consume(const std::stop_token& stop, std::size_t worker) {
    try {
        std::vector<IngestEvent> batch;
        batch.reserve(config_.consumer_batch);
        while (!stop.stop_requested()) {
            batch.clear();
            if (queue_.pop_batch(batch, config_.consumer_batch, stop) !=
                concurrency::PopStatus::kItem) {
                break;  // kClosed (drained) or kCancelled (deadline hit)
            }
            // process() is noexcept, so every event taken is counted. After a stop
            // request the event in hand is finished and the rest are handed back.
            std::size_t done = 0;
            while (done < batch.size() && (done == 0 || !stop.stop_requested())) {
                processor_.process(worker, batch[done], stop);
                ++done;
            }
            if (done < batch.size()) {
                const std::scoped_lock lock(unprocessed_mutex_);
                for (std::size_t i = done; i < batch.size(); ++i) {
                    unprocessed_.push_back(std::move(batch[i]));
                }
            }
        }
    } catch (...) {  // NOLINT(bugprone-empty-catch): only lock/allocation errors can reach here
        // No exception may escape a thread entry point (NFR-5).
    }
    {
        const std::scoped_lock lock(exit_mutex_);
        ++exited_;
    }
    exit_cv_.notify_all();
}

IngestEvent IngestPipeline::make_event(telemetry::RawTelemetrySample raw, std::uint64_t& sequence) {
    sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
    return IngestEvent{
        .sequence = sequence, .raw = std::move(raw), .submitted_at = SteadyClock::now()};
}

SubmitResult IngestPipeline::submit(telemetry::RawTelemetrySample raw, const std::stop_token& stop,
                                    concurrency::Deadline deadline) {
    std::uint64_t sequence = 0;
    IngestEvent event = make_event(std::move(raw), sequence);
    return to_submit_result(queue_.push(std::move(event), stop, deadline), sequence);
}

SubmitResult IngestPipeline::try_submit(telemetry::RawTelemetrySample raw) {
    std::uint64_t sequence = 0;
    IngestEvent event = make_event(std::move(raw), sequence);
    return to_submit_result(queue_.try_push(std::move(event)), sequence);
}

ShutdownReport IngestPipeline::shutdown(std::chrono::nanoseconds drain_timeout) {
    const std::scoped_lock guard(shutdown_mutex_);
    ShutdownReport report;
    if (shut_down_) {
        report.already_shut_down = true;
        report.drained = counters().in_queue == 0;
        report.counters = counters();
        return report;
    }

    const auto started = SteadyClock::now();
    const auto deadline =
        started + std::clamp(drain_timeout, std::chrono::nanoseconds{0}, kMaxDrainTimeout);

    // 1. Stop admission. Blocked producers wake up with kClosed and get their
    //    sample back; consumers keep draining and exit when the queue is empty.
    queue_.close();

    // 2. Wait for the consumers to drain, until the deadline.
    bool all_exited = false;
    {
        std::unique_lock lock(exit_mutex_);
        all_exited =
            exit_cv_.wait_until(lock, deadline, [this] { return exited_ == consumers_.size(); });
    }

    // 3. Deadline hit: cancel. Each consumer finishes the event it holds.
    if (!all_exited) {
        for (std::jthread& consumer : consumers_) {
            consumer.request_stop();
        }
    }
    for (std::jthread& consumer : consumers_) {
        if (consumer.joinable()) {
            consumer.join();
        }
    }

    // 4. Everything not processed (batch remainders held by cancelled consumers,
    //    then the queue) is handed back and counted, not destroyed.
    std::vector<IngestEvent> leftover;
    {
        const std::scoped_lock lock(unprocessed_mutex_);
        leftover = std::move(unprocessed_);
        unprocessed_.clear();
    }
    std::vector<IngestEvent> queued = queue_.take_all();
    leftover.insert(leftover.end(), std::make_move_iterator(queued.begin()),
                    std::make_move_iterator(queued.end()));
    abandoned_.fetch_add(leftover.size(), std::memory_order_relaxed);
    report.abandoned.reserve(leftover.size());
    for (IngestEvent& event : leftover) {
        report.abandoned.push_back(std::move(event.raw));
    }

    shut_down_ = true;
    report.drained = leftover.empty();
    report.elapsed = SteadyClock::now() - started;
    report.counters = counters();
    return report;
}

IngestCounters IngestPipeline::counters() const {
    // Read order matters for a consistent snapshot while running: processed and
    // abandoned are read before the queue's removed count, so in_flight >= 0.
    IngestCounters c;
    c.processing = processor_.counters();
    c.abandoned_at_shutdown = abandoned_.load(std::memory_order_relaxed);
    const concurrency::QueueStats q = queue_.stats();
    c.attempted = q.attempted;
    c.accepted = q.accepted;
    c.rejected_full = q.rejected_full;
    c.rejected_timeout = q.rejected_timeout;
    c.rejected_cancelled = q.rejected_cancelled;
    c.rejected_closed = q.rejected_closed;
    c.dropped_oldest = q.displaced_oldest;
    c.in_queue = q.depth;
    c.queue_high_watermark = q.high_watermark;
    c.queue_capacity = q.capacity;
    const std::uint64_t accounted = c.processed() + c.abandoned_at_shutdown;
    c.in_flight = q.removed >= accounted ? q.removed - accounted : 0;
    return c;
}

}  // namespace faulttwin::ingest
