#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

#include "faulttwin/concurrency/bounded_queue.hpp"
#include "faulttwin/concurrency/queue_types.hpp"

namespace faulttwin::concurrency {

/// REFERENCE DESIGN ONLY: the "just queue it" baseline for Experiment 2
/// (docs/evaluation/baselines.md). A std::deque guarded by a mutex that accepts
/// every push until it is closed, so under overload its memory grows without
/// limit. It exists so the bounded design can be compared against it; do not
/// use it in the ingest path.
template <typename T>
class UnboundedQueue {
  public:
    UnboundedQueue() { stats_.capacity = std::numeric_limits<std::size_t>::max(); }

    UnboundedQueue(const UnboundedQueue&) = delete;
    UnboundedQueue& operator=(const UnboundedQueue&) = delete;
    UnboundedQueue(UnboundedQueue&&) = delete;
    UnboundedQueue& operator=(UnboundedQueue&&) = delete;
    ~UnboundedQueue() = default;

    /// Never waits; `stop` and `deadline` exist only to satisfy QueueLike.
    PushResult<T> push(T item, const std::stop_token& /*stop*/ = {},
                       Deadline /*deadline*/ = kNoDeadline) {
        return try_push(std::move(item));
    }

    PushResult<T> try_push(T item) {
        bool wake = false;
        {
            const std::scoped_lock lock(mutex_);
            ++stats_.attempted;
            if (closed_) {
                ++stats_.rejected_closed;
                return {PushStatus::kClosed, std::move(item)};
            }
            items_.push_back(std::move(item));
            ++stats_.accepted;
            stats_.high_watermark = std::max(stats_.high_watermark, items_.size());
            wake = waiting_consumers_ > 0;
        }
        if (wake) {
            not_empty_.notify_one();
        }
        return {PushStatus::kAccepted, std::nullopt};
    }

    PopResult<T> pop(const std::stop_token& stop = {}, Deadline deadline = kNoDeadline) {
        std::unique_lock lock(mutex_);
        detail::wait_until(not_empty_, lock, waiting_consumers_, stop, deadline,
                           [this] { return !items_.empty() || closed_; });
        return pop_locked(stop);
    }

    PopResult<T> try_pop() {
        const std::scoped_lock lock(mutex_);
        return pop_locked({});
    }

    std::vector<T> take_all() {
        const std::scoped_lock lock(mutex_);
        std::vector<T> out;
        out.reserve(items_.size());
        for (T& item : items_) {
            out.push_back(std::move(item));
        }
        stats_.removed += items_.size();
        items_.clear();
        return out;
    }

    void close() {
        {
            const std::scoped_lock lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
    }

    [[nodiscard]] bool is_closed() const {
        const std::scoped_lock lock(mutex_);
        return closed_;
    }

    [[nodiscard]] QueueStats stats() const {
        const std::scoped_lock lock(mutex_);
        QueueStats copy = stats_;
        copy.depth = items_.size();
        return copy;
    }

  private:
    PopResult<T> pop_locked(const std::stop_token& stop) {
        if (!items_.empty()) {
            PopResult<T> result{PopStatus::kItem, std::move(items_.front())};
            items_.pop_front();
            ++stats_.removed;
            return result;
        }
        if (closed_) {
            return {PopStatus::kClosed, std::nullopt};
        }
        return {stop.stop_requested() ? PopStatus::kCancelled : PopStatus::kTimedOut, std::nullopt};
    }

    mutable std::mutex mutex_;
    std::condition_variable_any not_empty_;
    std::deque<T> items_;
    std::size_t waiting_consumers_{0};
    bool closed_{false};
    QueueStats stats_;
};

}  // namespace faulttwin::concurrency
