#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

#include "faulttwin/concurrency/bounded_queue.hpp"
#include "faulttwin/concurrency/queue_types.hpp"
#include "faulttwin/ingest/event_processor.hpp"
#include "faulttwin/telemetry/telemetry_sample.hpp"

namespace faulttwin::ingest {

struct PipelineConfig {
    std::size_t queue_capacity{1024};
    concurrency::OverloadPolicy overload{concurrency::OverloadPolicy::kBlock};
    std::size_t consumers{1};
    /// Maximum events a consumer takes from the queue per lock acquisition.
    /// 1 disables batching. On cancellation, untouched events of a batch are
    /// handed back with the abandoned events.
    std::size_t consumer_batch{64};
    ProcessingPolicy processing{};
    /// Used only if the pipeline is destroyed without an explicit shutdown().
    std::chrono::milliseconds destructor_drain_timeout{std::chrono::seconds{5}};
};

struct SubmitResult {
    concurrency::PushStatus status{concurrency::PushStatus::kAccepted};
    /// Ownership handed back to the producer: the refused sample, or for
    /// kAcceptedDisplacedOldest the older sample that was evicted.
    std::optional<telemetry::RawTelemetrySample> returned;
    std::uint64_t sequence{0};  ///< sequence number assigned to the submitted sample

    [[nodiscard]] bool accepted() const noexcept {
        return status == concurrency::PushStatus::kAccepted ||
               status == concurrency::PushStatus::kAcceptedDisplacedOldest;
    }
};

/// Every count the master specification asks for (ING-ATT/ACC/PROC/REJ/DROP,
/// docs/evaluation/metric-definitions.md §3) plus their breakdowns.
struct IngestCounters {
    // Admission (from the queue).
    std::uint64_t attempted{0};
    std::uint64_t accepted{0};
    std::uint64_t rejected_full{0};
    std::uint64_t rejected_timeout{0};
    std::uint64_t rejected_cancelled{0};
    std::uint64_t rejected_closed{0};
    std::uint64_t dropped_oldest{0};  ///< accepted, then evicted and handed back
    // Consumers.
    ProcessingCounters processing;
    // Shutdown.
    std::uint64_t abandoned_at_shutdown{0};  ///< accepted, unprocessed at the drain deadline
    // Current state.
    std::uint64_t in_queue{0};
    std::uint64_t in_flight{0};  ///< taken by a consumer, not yet counted as processed
    std::size_t queue_high_watermark{0};
    std::size_t queue_capacity{0};

    [[nodiscard]] std::uint64_t rejected() const noexcept {
        return rejected_full + rejected_timeout + rejected_cancelled + rejected_closed;
    }
    [[nodiscard]] std::uint64_t processed() const noexcept { return processing.processed(); }

    /// ATT = ACC + REJ and ACC = PROC + DROP + ABANDONED + in-queue + in-flight.
    [[nodiscard]] bool invariants_hold() const noexcept {
        return attempted == accepted + rejected() && accepted == processed() + dropped_oldest +
                                                                     abandoned_at_shutdown +
                                                                     in_queue + in_flight;
    }
};

struct ShutdownReport {
    bool already_shut_down{false};  ///< true for every call after the first
    bool drained{false};            ///< every accepted event was processed before the deadline
    std::chrono::nanoseconds elapsed{0};
    /// Accepted but unprocessed when the deadline expired, handed back so that
    /// the caller can persist, retry or report them. Empty when drained.
    std::vector<telemetry::RawTelemetrySample> abandoned;
    IngestCounters counters;
};

/// Concurrent ingest pipeline: producers call submit() from any thread; a fixed
/// pool of consumer std::jthreads pops events from a BoundedQueue and runs the
/// shared EventProcessor (validate, order check, policy, sink).
///
/// Lifecycle: consumers start in the constructor. shutdown(timeout) closes
/// admission, lets consumers drain until the deadline, then cancels them via
/// their stop tokens (each finishes the event it holds), joins them and hands
/// back whatever is still queued. Nothing is discarded without being counted.
class IngestPipeline {
  public:
    /// Throws std::invalid_argument for 0 consumers, 0 capacity, 0 batch size or
    /// an empty sink.
    IngestPipeline(const PipelineConfig& config, NowFn now, EventSink sink);

    IngestPipeline(const IngestPipeline&) = delete;
    IngestPipeline& operator=(const IngestPipeline&) = delete;
    IngestPipeline(IngestPipeline&&) = delete;
    IngestPipeline& operator=(IngestPipeline&&) = delete;

    /// Calls shutdown(destructor_drain_timeout) if it was not called. Events it
    /// abandons are counted and then destroyed; call shutdown() to keep them.
    ~IngestPipeline();

    /// Thread-safe. Applies the overload policy; with kBlock waits until
    /// `deadline`, a stop request on `stop`, or shutdown.
    SubmitResult submit(telemetry::RawTelemetrySample raw, const std::stop_token& stop = {},
                        concurrency::Deadline deadline = concurrency::kNoDeadline);

    /// Thread-safe; never waits.
    SubmitResult try_submit(telemetry::RawTelemetrySample raw);

    /// Thread-safe and idempotent. drain_timeout is capped at 24 hours.
    ShutdownReport shutdown(std::chrono::nanoseconds drain_timeout);

    /// Thread-safe. While consumers run, the snapshot is approximate (fields
    /// are read one after another); after shutdown() it is exact.
    [[nodiscard]] IngestCounters counters() const;

    [[nodiscard]] const PipelineConfig& config() const noexcept { return config_; }

  private:
    void consume(const std::stop_token& stop, std::size_t worker);
    IngestEvent make_event(telemetry::RawTelemetrySample raw, std::uint64_t& sequence);

    PipelineConfig config_;
    EventProcessor processor_;
    concurrency::BoundedQueue<IngestEvent> queue_;
    std::atomic<std::uint64_t> next_sequence_{0};
    std::atomic<std::uint64_t> abandoned_{0};

    std::mutex exit_mutex_;
    std::condition_variable exit_cv_;
    std::size_t exited_{0};

    std::mutex shutdown_mutex_;
    bool shut_down_{false};

    std::mutex unprocessed_mutex_;
    std::vector<IngestEvent> unprocessed_;  ///< batch remainders left by cancelled consumers

    std::vector<std::jthread> consumers_;  // last: started after every other member exists
};

}  // namespace faulttwin::ingest
