#include "faulttwin/ingest/event_processor.hpp"

#include <numeric>
#include <stdexcept>
#include <utility>

namespace faulttwin::ingest {

std::string_view to_string(DuplicatePolicy policy) noexcept {
    switch (policy) {
        case DuplicatePolicy::kReject:
            return "reject";
        case DuplicatePolicy::kAccept:
            return "accept";
    }
    return "invalid";
}

std::string_view to_string(OutOfOrderPolicy policy) noexcept {
    switch (policy) {
        case OutOfOrderPolicy::kAccept:
            return "accept";
        case OutOfOrderPolicy::kReject:
            return "reject";
    }
    return "invalid";
}

std::string_view to_string(Disposition disposition) noexcept {
    switch (disposition) {
        case Disposition::kDelivered:
            return "delivered";
        case Disposition::kDeliveredOutOfOrder:
            return "delivered-out-of-order";
        case Disposition::kDeliveredDuplicate:
            return "delivered-duplicate";
        case Disposition::kRejectedInvalid:
            return "rejected-invalid";
        case Disposition::kRejectedDuplicate:
            return "rejected-duplicate";
        case Disposition::kRejectedOutOfOrder:
            return "rejected-out-of-order";
        case Disposition::kRejectedNodeLimit:
            return "rejected-node-limit";
        case Disposition::kFailed:
            return "failed";
    }
    return "invalid";
}

std::uint64_t ProcessingCounters::processed() const noexcept {
    return std::accumulate(by_disposition.begin(), by_disposition.end(), std::uint64_t{0});
}

std::uint64_t ProcessingCounters::delivered() const noexcept {
    return count(Disposition::kDelivered) + count(Disposition::kDeliveredOutOfOrder) +
           count(Disposition::kDeliveredDuplicate);
}

namespace {

std::size_t checked_workers(std::size_t workers) {
    if (workers == 0) {
        throw std::invalid_argument("EventProcessor needs at least one worker");
    }
    return workers;
}

EventSink checked_sink(EventSink sink) {
    if (!sink) {
        throw std::invalid_argument("EventProcessor needs a sink");
    }
    return sink;
}

}  // namespace

EventProcessor::EventProcessor(const ProcessingPolicy& policy, std::size_t workers, NowFn now,
                               EventSink sink)
    : policy_(policy),
      workers_(checked_workers(workers)),
      now_(now ? std::move(now) : NowFn{[] { return telemetry::Clock::now(); }}),
      sink_(checked_sink(std::move(sink))),
      tracker_(policy.ordering),
      counters_(std::make_unique<WorkerCounters[]>(workers)) {}  // NOLINT(*-avoid-c-arrays)

Disposition EventProcessor::classify_and_deliver(std::size_t worker, const IngestEvent& event,
                                                 const std::stop_token& stop) {
    const telemetry::ValidationResult result =
        telemetry::validate_sample(event.raw, now_(), policy_.validation);

    Disposition disposition = Disposition::kRejectedInvalid;
    if (result.sample.has_value()) {
        const telemetry::TelemetrySample& sample = *result.sample;
        switch (tracker_.observe(sample.node_id(), sample.collected_at())) {
            case OrderVerdict::kInOrder:
                disposition = Disposition::kDelivered;
                break;
            case OrderVerdict::kOutOfOrder:
                disposition = policy_.out_of_order == OutOfOrderPolicy::kAccept
                                  ? Disposition::kDeliveredOutOfOrder
                                  : Disposition::kRejectedOutOfOrder;
                break;
            case OrderVerdict::kDuplicate:
                disposition = policy_.duplicates == DuplicatePolicy::kAccept
                                  ? Disposition::kDeliveredDuplicate
                                  : Disposition::kRejectedDuplicate;
                break;
            case OrderVerdict::kNodeLimitExceeded:
                disposition = Disposition::kRejectedNodeLimit;
                break;
        }
    }

    const ProcessedEvent view{
        .event = &event,
        .disposition = disposition,
        .sample =
            result.sample.has_value() && is_delivered(disposition) ? &*result.sample : nullptr,
        .issues = result.issues,
        .stop = stop,
    };
    sink_(worker, view);
    return disposition;
}

Disposition EventProcessor::process(std::size_t worker, const IngestEvent& event,
                                    const std::stop_token& stop) noexcept {
    Disposition disposition = Disposition::kFailed;
    try {
        disposition = classify_and_deliver(worker, event, stop);
    } catch (...) {
        // A throwing sink (or std::bad_alloc in validation) must not kill the
        // consumer thread. The event is counted as failed, never lost silently.
        disposition = Disposition::kFailed;
    }
    counters_[worker]
        .by_disposition.at(static_cast<std::size_t>(disposition))
        .fetch_add(1, std::memory_order_relaxed);
    return disposition;
}

ProcessingCounters EventProcessor::counters() const noexcept {
    ProcessingCounters total;
    for (std::size_t w = 0; w < workers_; ++w) {
        for (std::size_t d = 0; d < kDispositionCount; ++d) {
            total.by_disposition.at(d) +=
                counters_[w].by_disposition.at(d).load(std::memory_order_relaxed);
        }
    }
    return total;
}

}  // namespace faulttwin::ingest
