# ADR-0006: Mutex-based bounded queue first; lock-free only if measured

- Status: Accepted (Phase 2)
- Date: 2026-11-25 (proposed in Phase 0, accepted with the Phase 2 implementation)

## Context
The ingest path needs a bounded MPMC queue with backpressure. Lock-free queues are faster under
contention but harder to verify and to make TSan-clean, and blocking semantics (wait for space) need
extra machinery.

## Decision
`faulttwin::concurrency::BoundedQueue<T>`: a fixed ring buffer (allocated once) guarded by one
`std::mutex` and two `std::condition_variable_any`s (the `_any` variant is needed for C++20
`std::stop_token` waits).

- **Overload policies:** `kBlock` (wait until a deadline, a stop request or close),
  `kRejectNewest` (refuse at once) and `kDropOldest` (evict the oldest). The Phase 0 name
  `drop_newest` became `kRejectNewest`: the new item is refused and *handed back*, not dropped.
- **Nothing is destroyed by the queue.** Every refused or evicted item is returned to the caller in
  `PushResult::item`, and every outcome is counted in `QueueStats` under the lock, so
  `attempted = accepted + rejected` and `accepted = removed + displaced + depth` always hold.
- **Timeouts** use `std::chrono::steady_clock` deadlines. **Cancellation** uses `std::stop_token`.
  `close()` is the graceful-shutdown signal: producers get `kClosed` with their item; consumers
  drain, then get `kClosed`.
- **Wake-up policy (added after the first measurements).** Notifies are skipped when nobody waits
  (waiter counts kept under the lock), consumers are woken on the empty → non-empty edge, producers
  on falling to the low-water mark (half the capacity), and a woken thread that leaves work behind
  wakes one more ("cascade"). `pop_batch()` lets a consumer take up to N items per lock acquisition.
  See the Phase 2 report §6 and §17 for the measured effect of each change.
- `T` must be nothrow-move-constructible, so every operation is strongly exception safe.
- `QueueLike` concept: `BoundedQueue` and the reference `UnboundedQueue` (Experiment 2 baseline)
  both satisfy it, so a lock-free implementation (e.g. Vyukov bounded MPMC) can be benchmarked
  against it in Phase 11 and swapped only if it is the measured bottleneck.

## Consequences
- Simple correctness story, TSan-clean, and benchmarks provide the comparison for interviews.
- With very cheap per-event work the mutex hand-off and cross-core cache traffic dominate, so the
  concurrent pipeline is slower than a single thread (measured, Phase 2 report §16). This is the
  expected cost of a mutex queue, not hidden.
- No fairness between waiting producers is promised.
