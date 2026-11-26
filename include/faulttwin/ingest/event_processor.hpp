#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <string_view>

#include "faulttwin/ingest/order_tracker.hpp"
#include "faulttwin/telemetry/telemetry_sample.hpp"
#include "faulttwin/telemetry/validation.hpp"

namespace faulttwin::ingest {

using SteadyClock = std::chrono::steady_clock;

/// One unit of ingest work: an untrusted sample plus bookkeeping.
struct IngestEvent {
    std::uint64_t sequence{0};             ///< submission order, assigned by the producer side
    telemetry::RawTelemetrySample raw;     ///< untrusted input
    SteadyClock::time_point submitted_at;  ///< when the producer offered it (latency start)
};

enum class DuplicatePolicy : std::uint8_t {
    kReject,  ///< default: a repeated (node, collected_at) is counted and not delivered
    kAccept,  ///< counted and delivered as kDeliveredDuplicate
};

enum class OutOfOrderPolicy : std::uint8_t {
    kAccept,  ///< default: late data is still real data; counted and delivered
    kReject,  ///< counted and not delivered
};

[[nodiscard]] std::string_view to_string(DuplicatePolicy policy) noexcept;
[[nodiscard]] std::string_view to_string(OutOfOrderPolicy policy) noexcept;

/// Final outcome of one event that a consumer handled. Every handled event has
/// exactly one disposition and is passed to the sink exactly once.
enum class Disposition : std::uint8_t {
    kDelivered,            ///< valid, in order
    kDeliveredOutOfOrder,  ///< valid, older than the node's newest (OutOfOrderPolicy::kAccept)
    kDeliveredDuplicate,   ///< valid, repeated (DuplicatePolicy::kAccept)
    kRejectedInvalid,      ///< failed validate_sample; issues are attached
    kRejectedDuplicate,    ///< DuplicatePolicy::kReject
    kRejectedOutOfOrder,   ///< OutOfOrderPolicy::kReject
    kRejectedNodeLimit,    ///< NodeOrderTracker is full; see OrderTrackerConfig::max_nodes
    kFailed,               ///< the sink or the processing threw; the event was not handled
};

inline constexpr std::size_t kDispositionCount = 8;

[[nodiscard]] std::string_view to_string(Disposition disposition) noexcept;
[[nodiscard]] constexpr bool is_delivered(Disposition d) noexcept {
    return d == Disposition::kDelivered || d == Disposition::kDeliveredOutOfOrder ||
           d == Disposition::kDeliveredDuplicate;
}

/// What the sink sees. Pointers and spans are only valid during the sink call.
struct ProcessedEvent {
    const IngestEvent* event{nullptr};  ///< never null
    Disposition disposition{Disposition::kDelivered};
    const telemetry::TelemetrySample* sample{nullptr};   ///< non-null iff is_delivered()
    std::span<const telemetry::ValidationIssue> issues;  ///< non-empty iff kRejectedInvalid
    std::stop_token stop;  ///< shutdown cancellation, for sinks that do long work
};

/// Receives every handled event, delivered or rejected (the dead-letter path
/// is the same call). Called concurrently from consumer threads with distinct
/// `worker` indices in [0, workers), so per-worker state needs no locking.
/// A sink must not block indefinitely without observing ProcessedEvent::stop.
using EventSink = std::function<void(std::size_t worker, const ProcessedEvent&)>;

/// Source of "now" for timestamp validation; injectable for deterministic tests.
using NowFn = std::function<telemetry::Timestamp()>;

struct ProcessingPolicy {
    telemetry::ValidationPolicy validation{};
    DuplicatePolicy duplicates{DuplicatePolicy::kReject};
    OutOfOrderPolicy out_of_order{OutOfOrderPolicy::kAccept};
    OrderTrackerConfig ordering{};
};

struct ProcessingCounters {
    std::array<std::uint64_t, kDispositionCount> by_disposition{};

    [[nodiscard]] std::uint64_t count(Disposition d) const noexcept {
        return by_disposition.at(static_cast<std::size_t>(d));
    }
    [[nodiscard]] std::uint64_t processed() const noexcept;  ///< sum over all dispositions
    [[nodiscard]] std::uint64_t delivered() const noexcept;  ///< the three kDelivered* values
};

/// The per-event work shared by the concurrent pipeline and the single-threaded
/// baseline, so both do identical work (docs/evaluation/baselines.md):
/// validate -> order/duplicate check -> apply policy -> sink.
class EventProcessor {
  public:
    /// Throws std::invalid_argument if workers == 0 or sink is empty.
    EventProcessor(const ProcessingPolicy& policy, std::size_t workers, NowFn now, EventSink sink);

    /// Thread-safe for distinct `worker` values. Never throws: failures become
    /// Disposition::kFailed. Precondition: worker < workers().
    Disposition process(std::size_t worker, const IngestEvent& event,
                        const std::stop_token& stop = {}) noexcept;

    /// Sum of the per-worker counters. Values are read individually (relaxed),
    /// so a snapshot taken while consumers run may be slightly behind.
    [[nodiscard]] ProcessingCounters counters() const noexcept;

    [[nodiscard]] std::size_t workers() const noexcept { return workers_; }
    [[nodiscard]] const ProcessingPolicy& policy() const noexcept { return policy_; }

  private:
    // One cache line per worker so that consumers do not share counter lines.
    struct alignas(64) WorkerCounters {
        std::array<std::atomic<std::uint64_t>, kDispositionCount> by_disposition{};
    };

    Disposition classify_and_deliver(std::size_t worker, const IngestEvent& event,
                                     const std::stop_token& stop);

    ProcessingPolicy policy_;
    std::size_t workers_;
    NowFn now_;
    EventSink sink_;
    NodeOrderTracker tracker_;
    std::unique_ptr<WorkerCounters[]> counters_;  // NOLINT(*-avoid-c-arrays): atomics not movable
};

}  // namespace faulttwin::ingest
