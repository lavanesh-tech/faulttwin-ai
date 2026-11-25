# ADR-0006: Mutex-based bounded queue first; lock-free only if measured

- Status: Proposed
- Date: 2026-11-25

## Context
The ingest path needs a bounded MPMC queue with backpressure. Lock-free queues are faster under
contention but harder to verify and to make TSan-clean, and blocking semantics (wait for space) need
extra machinery.

## Decision
Implement a ring buffer guarded by a mutex with two condition variables, supporting `block`,
`drop_oldest`, `drop_newest`, close and timed waits. Define a `BoundedQueueLike` concept so a
lock-free implementation (Vyukov bounded MPMC) can be benchmarked against it in Phase 11 and swapped
only if it is the measured bottleneck.

## Consequences
- Simpler correctness story; benchmarks provide the comparison for interviews.
