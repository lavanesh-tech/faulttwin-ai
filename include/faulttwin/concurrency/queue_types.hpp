#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string_view>
#include <utility>
#include <vector>

namespace faulttwin::concurrency {

/// Monotonic clock for every timeout and deadline in this module. Wall-clock
/// time (system_clock) can jump, which would make timeouts wrong.
using QueueClock = std::chrono::steady_clock;
using Deadline = QueueClock::time_point;

/// Sentinel meaning "no deadline": wait until success, close or cancellation.
inline constexpr Deadline kNoDeadline = Deadline::max();

/// What a bounded queue does when a producer offers an item and the queue is full.
enum class OverloadPolicy : std::uint8_t {
    kBlock,         ///< wait for space until the deadline, cancellation or close
    kRejectNewest,  ///< refuse the offered item immediately and hand it back
    kDropOldest,    ///< evict the oldest queued item, hand it back, store the new one
};

[[nodiscard]] std::string_view to_string(OverloadPolicy policy) noexcept;
[[nodiscard]] std::optional<OverloadPolicy> parse_overload_policy(std::string_view text) noexcept;

/// Outcome of a push. Every status except kAccepted hands an item back to the
/// caller in PushResult::item, so a queue never destroys work on its own.
enum class PushStatus : std::uint8_t {
    kAccepted,                 ///< stored; nothing displaced
    kAcceptedDisplacedOldest,  ///< stored; the evicted oldest item is returned
    kRejectedFull,             ///< full and not allowed to wait; the offered item is returned
    kTimedOut,                 ///< kBlock: no space before the deadline; offered item returned
    kCancelled,                ///< kBlock: stop requested while waiting; offered item returned
    kClosed,                   ///< queue closed; offered item returned
};

[[nodiscard]] std::string_view to_string(PushStatus status) noexcept;

template <typename T>
struct PushResult {
    PushStatus status{PushStatus::kAccepted};
    /// Engaged for every status except kAccepted: the caller's own item when it
    /// was refused, or the displaced oldest item for kAcceptedDisplacedOldest.
    std::optional<T> item;

    [[nodiscard]] bool accepted() const noexcept {
        return status == PushStatus::kAccepted || status == PushStatus::kAcceptedDisplacedOldest;
    }
};

enum class PopStatus : std::uint8_t {
    kItem,       ///< PopResult::item is engaged
    kTimedOut,   ///< empty until the deadline
    kCancelled,  ///< stop requested while waiting on an empty queue
    kClosed,     ///< closed and fully drained: no item will ever arrive
};

[[nodiscard]] std::string_view to_string(PopStatus status) noexcept;

template <typename T>
struct PopResult {
    PopStatus status{PopStatus::kClosed};
    std::optional<T> item;  ///< engaged if and only if status == kItem
};

/// Consistent snapshot of a queue's counters, taken under the queue's lock.
struct QueueStats {
    std::uint64_t attempted{0};           ///< push calls of any kind
    std::uint64_t accepted{0};            ///< items stored
    std::uint64_t rejected_full{0};       ///< refused because full (no wait allowed)
    std::uint64_t rejected_timeout{0};    ///< refused after waiting until the deadline
    std::uint64_t rejected_cancelled{0};  ///< refused because stop was requested
    std::uint64_t rejected_closed{0};     ///< refused because the queue was closed
    std::uint64_t displaced_oldest{0};    ///< accepted items later evicted by kDropOldest
    std::uint64_t removed{0};             ///< items handed out by pop/try_pop/take_all
    std::size_t depth{0};                 ///< items currently stored
    std::size_t high_watermark{0};        ///< largest depth ever observed
    std::size_t capacity{0};              ///< SIZE_MAX for the unbounded reference queue

    [[nodiscard]] std::uint64_t rejected() const noexcept {
        return rejected_full + rejected_timeout + rejected_cancelled + rejected_closed;
    }

    /// attempted = accepted + rejected, and every accepted item is either still
    /// stored, handed out, or evicted (and handed back to the producer).
    [[nodiscard]] bool invariants_hold() const noexcept {
        return attempted == accepted + rejected() &&
               accepted == removed + displaced_oldest + static_cast<std::uint64_t>(depth) &&
               depth <= capacity && high_watermark <= capacity;
    }
};

/// The interface shared by BoundedQueue and the UnboundedQueue reference
/// design, so that benchmarks (and a future lock-free queue, ADR-0006) can be
/// swapped without touching callers.
template <typename Q, typename T>
concept QueueLike = requires(Q& queue, T item, std::stop_token stop, Deadline deadline) {
    { queue.push(std::move(item), stop, deadline) } -> std::same_as<PushResult<T>>;
    { queue.try_push(std::move(item)) } -> std::same_as<PushResult<T>>;
    { queue.pop(stop, deadline) } -> std::same_as<PopResult<T>>;
    { queue.try_pop() } -> std::same_as<PopResult<T>>;
    { queue.take_all() } -> std::same_as<std::vector<T>>;
    queue.close();
    { queue.is_closed() } -> std::same_as<bool>;
    { queue.stats() } -> std::same_as<QueueStats>;
};

}  // namespace faulttwin::concurrency
