#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <type_traits>
#include <utility>
#include <vector>

#include "faulttwin/concurrency/queue_types.hpp"

namespace faulttwin::concurrency {

namespace detail {

/// Waits on `cv` until `pred` holds, `stop` is requested or `deadline` passes.
/// Returns the final value of `pred`, like std::condition_variable_any::wait.
///
/// `waiters` (guarded by the same mutex as `lock`) counts threads inside the
/// wait, so that notifiers can skip the notify call, which is a system call on
/// most platforms, when nobody is waiting. A waiter increments the count while
/// holding the lock, before the wait releases it, so a notifier that reads the
/// count under the lock can never miss a waiter.
template <typename Pred>
bool wait_until(std::condition_variable_any& cv, std::unique_lock<std::mutex>& lock,
                std::size_t& waiters, const std::stop_token& stop, Deadline deadline, Pred pred) {
    if (pred()) {
        return true;
    }
    ++waiters;
    // A time_point::max() deadline is not passed to wait_until: some standard
    // library versions convert it to another clock and overflow.
    const bool satisfied = deadline == kNoDeadline
                               ? cv.wait(lock, stop, std::move(pred))
                               : cv.wait_until(lock, stop, deadline, std::move(pred));
    --waiters;
    return satisfied;
}

}  // namespace detail

/// Bounded multi-producer / multi-consumer FIFO queue (ADR-0006).
///
/// A fixed ring buffer guarded by one mutex and two condition variables. The
/// buffer is allocated once, in the constructor, so memory held by the queue
/// itself never grows. Every refused or evicted item is handed back to the
/// caller in the PushResult, so the queue never destroys work silently, and
/// every outcome is counted in QueueStats.
///
/// Cancellation uses std::stop_token and timeouts use the monotonic
/// QueueClock. close() is the graceful-shutdown signal: producers are refused,
/// consumers keep receiving items until the queue is empty, then kClosed.
///
/// T must be nothrow-move-constructible, which keeps every operation strongly
/// exception safe (the only allocation happens in the constructor).
template <typename T>
class BoundedQueue {
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "BoundedQueue requires a nothrow move constructor");

  public:
    /// Throws std::invalid_argument if capacity is 0 (a configuration error).
    BoundedQueue(std::size_t capacity, OverloadPolicy policy)
        : slots_(checked_capacity(capacity)), policy_(policy) {
        stats_.capacity = capacity;
    }

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;
    BoundedQueue(BoundedQueue&&) = delete;
    BoundedQueue& operator=(BoundedQueue&&) = delete;
    ~BoundedQueue() = default;

    /// Offers an item using the configured overload policy. Only kBlock waits,
    /// and only until `deadline`, a stop request on `stop`, or close().
    PushResult<T> push(T item, const std::stop_token& stop = {}, Deadline deadline = kNoDeadline) {
        return push_impl(std::move(item), /*may_wait=*/true, stop, deadline);
    }

    /// Never waits. With kBlock a full queue yields kRejectedFull.
    PushResult<T> try_push(T item) {
        return push_impl(std::move(item), /*may_wait=*/false, {}, kNoDeadline);
    }

    /// Waits for an item until `deadline`, a stop request, or close-and-empty.
    /// An item that is already available is returned even if stop was requested.
    PopResult<T> pop(const std::stop_token& stop = {}, Deadline deadline = kNoDeadline) {
        std::unique_lock lock(mutex_);
        detail::wait_until(not_empty_, lock, waiting_consumers_, stop, deadline,
                           [this] { return size_ > 0 || closed_; });
        return pop_locked(lock, stop);
    }

    /// Like pop(), but moves up to `max_items` items (at least one on kItem) into
    /// `out` under a single lock acquisition. Batching cuts lock hand-offs and
    /// wake-ups per item. Items are appended in FIFO order; `out` is not cleared.
    PopStatus pop_batch(std::vector<T>& out, std::size_t max_items,
                        const std::stop_token& stop = {}, Deadline deadline = kNoDeadline) {
        std::unique_lock lock(mutex_);
        detail::wait_until(not_empty_, lock, waiting_consumers_, stop, deadline,
                           [this] { return size_ > 0 || closed_; });
        if (size_ == 0) {
            return pop_locked(lock, stop).status;
        }
        const std::size_t n = std::min(size_, std::max<std::size_t>(max_items, 1));
        out.reserve(out.size() + n);  // may throw; nothing has been removed yet
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back(take_front_locked());
        }
        stats_.removed += n;
        notify_after_pop(lock, /*size_before=*/size_ + n);
        return PopStatus::kItem;
    }

    /// Never waits.
    PopResult<T> try_pop() {
        std::unique_lock lock(mutex_);
        return pop_locked(lock, {});
    }

    /// Removes and returns every stored item (counted as removed). Used by
    /// shutdown to account for work that was not processed before a deadline.
    std::vector<T> take_all() {
        std::vector<T> items;
        {
            const std::scoped_lock lock(mutex_);
            items.reserve(size_);
            while (size_ > 0) {
                items.push_back(take_front_locked());
                ++stats_.removed;
            }
        }
        not_full_.notify_all();
        return items;
    }

    /// Idempotent. Wakes every waiting producer (refused with kClosed) and
    /// consumer (which drains the remaining items, then gets kClosed).
    void close() {
        {
            const std::scoped_lock lock(mutex_);
            closed_ = true;
        }
        not_full_.notify_all();
        not_empty_.notify_all();
    }

    [[nodiscard]] bool is_closed() const {
        const std::scoped_lock lock(mutex_);
        return closed_;
    }

    [[nodiscard]] QueueStats stats() const {
        const std::scoped_lock lock(mutex_);
        QueueStats copy = stats_;
        copy.depth = size_;
        return copy;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
    [[nodiscard]] OverloadPolicy policy() const noexcept { return policy_; }

  private:
    static std::size_t checked_capacity(std::size_t capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("BoundedQueue capacity must be at least 1");
        }
        return capacity;
    }

    PushResult<T> push_impl(T item, bool may_wait, const std::stop_token& stop, Deadline deadline) {
        std::unique_lock lock(mutex_);
        ++stats_.attempted;
        if (closed_) {
            ++stats_.rejected_closed;
            return {PushStatus::kClosed, std::move(item)};
        }
        if (size_ < slots_.size()) {
            return store_and_notify(lock, std::move(item));
        }

        switch (policy_) {
            case OverloadPolicy::kRejectNewest:
                ++stats_.rejected_full;
                return {PushStatus::kRejectedFull, std::move(item)};
            case OverloadPolicy::kDropOldest: {
                T oldest = take_front_locked();
                ++stats_.displaced_oldest;
                store_locked(std::move(item));
                notify_after_push(lock);
                return {PushStatus::kAcceptedDisplacedOldest, std::move(oldest)};
            }
            case OverloadPolicy::kBlock:
                break;
        }

        if (!may_wait) {
            ++stats_.rejected_full;
            return {PushStatus::kRejectedFull, std::move(item)};
        }
        const bool has_space =
            detail::wait_until(not_full_, lock, waiting_producers_, stop, deadline,
                               [this] { return closed_ || size_ < slots_.size(); });
        // Close wins over everything: a closed queue must not accept new work.
        if (closed_) {
            ++stats_.rejected_closed;
            return {PushStatus::kClosed, std::move(item)};
        }
        if (!has_space) {
            if (stop.stop_requested()) {
                ++stats_.rejected_cancelled;
                return {PushStatus::kCancelled, std::move(item)};
            }
            ++stats_.rejected_timeout;
            return {PushStatus::kTimedOut, std::move(item)};
        }
        return store_and_notify(lock, std::move(item));
    }

    PushResult<T> store_and_notify(std::unique_lock<std::mutex>& lock, T item) {
        store_locked(std::move(item));
        notify_after_push(lock);
        return {PushStatus::kAccepted, std::nullopt};
    }

    // ---- Wake-up policy ------------------------------------------------------
    // A notify is a system call on most platforms, so it is issued only when it
    // can matter, decided under the lock and sent after unlocking:
    //  - a push wakes one consumer when the queue goes from empty to non-empty;
    //  - a pop wakes one producer when the depth falls to the low-water mark
    //    (half the capacity). Producers only wait on a full queue, so the depth
    //    always crosses that mark before the queue empties. Waking at half
    //    instead of at "one slot free" avoids a producer/consumer ping-pong
    //    with one wake-up per item when producers are faster (hysteresis);
    //  - a thread that leaves work behind (items after a pop, space after a
    //    push) and sees more waiters wakes one more ("cascade").
    // Together these keep the invariant that whenever items (or free slots)
    // exist and consumers (or producers) wait, at least one of them is awake or
    // signalled, so no wake-up is lost. close() and take_all() wake everyone.

    void notify_after_push(std::unique_lock<std::mutex>& lock) {
        const bool wake_consumer = waiting_consumers_ > 0 && size_ == 1;
        const bool wake_producer = waiting_producers_ > 0 && size_ < slots_.size();
        lock.unlock();
        if (wake_consumer) {
            not_empty_.notify_one();
        }
        if (wake_producer) {
            not_full_.notify_one();
        }
    }

    void notify_after_pop(std::unique_lock<std::mutex>& lock, std::size_t size_before) {
        const std::size_t low_water = slots_.size() / 2;
        const bool wake_producer =
            waiting_producers_ > 0 && size_before > low_water && size_ <= low_water;
        const bool wake_consumer = waiting_consumers_ > 0 && size_ > 0;
        lock.unlock();
        if (wake_producer) {
            not_full_.notify_one();
        }
        if (wake_consumer) {
            not_empty_.notify_one();
        }
    }

    PopResult<T> pop_locked(std::unique_lock<std::mutex>& lock, const std::stop_token& stop) {
        if (size_ > 0) {
            T item = take_front_locked();
            ++stats_.removed;
            notify_after_pop(lock, /*size_before=*/size_ + 1);
            return {PopStatus::kItem, std::move(item)};
        }
        if (closed_) {
            return {PopStatus::kClosed, std::nullopt};
        }
        if (stop.stop_requested()) {
            return {PopStatus::kCancelled, std::nullopt};
        }
        return {PopStatus::kTimedOut, std::nullopt};
    }

    void store_locked(T item) noexcept {
        slots_[(head_ + size_) % slots_.size()].emplace(std::move(item));
        ++size_;
        ++stats_.accepted;
        stats_.high_watermark = std::max(stats_.high_watermark, size_);
    }

    T take_front_locked() noexcept {
        std::optional<T>& slot = slots_[head_];
        T item = std::move(*slot);  // NOLINT(bugprone-unchecked-optional-access): size_ > 0
        slot.reset();
        head_ = (head_ + 1) % slots_.size();
        --size_;
        return item;
    }

    mutable std::mutex mutex_;
    std::condition_variable_any not_empty_;
    std::condition_variable_any not_full_;
    std::vector<std::optional<T>> slots_;
    std::size_t head_{0};
    std::size_t size_{0};
    std::size_t waiting_consumers_{0};
    std::size_t waiting_producers_{0};
    bool closed_{false};
    OverloadPolicy policy_;
    QueueStats stats_;
};

}  // namespace faulttwin::concurrency
