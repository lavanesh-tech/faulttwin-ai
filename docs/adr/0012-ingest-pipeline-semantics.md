# ADR-0012: Ingest pipeline semantics — dispositions, ordering policies, ownership hand-back, shutdown

- Status: Accepted (Phase 2)
- Date: 2026-11-25

## Context
The master specification requires producers and consumers over a bounded queue, explicit overload
policies, graceful shutdown, cancellation and timeouts, counting of attempted, accepted, processed,
rejected and dropped events, and that work is never discarded silently. The acceptance criteria add
duplicate, invalid and out-of-order policies.

## Decision
1. **One shared unit of work.** `EventProcessor` does validate → order/duplicate check → policy →
   sink. The concurrent `IngestPipeline` and the single-threaded baseline both use it, so
   Experiment 1 compares equal work.
2. **Every handled event gets exactly one `Disposition`** and is passed to the sink exactly once:
   `delivered`, `delivered-out-of-order`, `delivered-duplicate`, `rejected-invalid` (with the
   validation issues), `rejected-duplicate`, `rejected-out-of-order`, `rejected-node-limit`,
   `failed` (the sink or processing threw). Rejected events go through the same sink call, which
   is the dead-letter path. Counters are per worker (one cache line each) and summed on read.
3. **Ordering identity is (node id, `collected_at`).** `NodeOrderTracker` keeps, per node, the
   newest time and a ring of the last 64 times (striped over 16 mutexes, at most 1,024 nodes).
   Defaults: duplicates **rejected**, out-of-order **accepted** (late data is still real data; it is
   counted and flagged). A duplicate older than the 64-entry window is reported as out-of-order.
   With several consumers, events of one node can be processed in a different order than they were
   queued, so "out of order" can be caused by the pipeline itself; consumers do not shard by node
   in Phase 2.
4. **Ownership goes back to the producer.** `submit()` returns the refused sample (or, for
   `kDropOldest`, the evicted older sample) in `SubmitResult::returned`. The pipeline never
   destroys a sample it did not process.
5. **Shutdown with a drain deadline.** `shutdown(timeout)`: close admission (blocked producers get
   `kClosed` and their sample back) → consumers drain until the deadline → if the deadline passes,
   `request_stop()` on every consumer `std::jthread`; each finishes the event it holds, and hands
   back the rest of its batch → join → everything not processed is returned in
   `ShutdownReport::abandoned` and counted. The sink receives the consumer's `stop_token`, so long
   sink work can observe cancellation. The destructor calls `shutdown(5 s)` as a safety net.
6. **Invariants**, checked by tests and by every benchmark run:
   `attempted = accepted + rejected` and
   `accepted = processed + dropped + abandoned + in_queue + in_flight`.
7. **No exception escapes a consumer thread** (NFR-5). `process()` is `noexcept`.

## Alternatives considered
- Dropping refused events inside the pipeline and only counting them: satisfies "counted" but not
  "never discarded silently" from the producer's point of view.
- Sharding consumers by node to preserve per-node order: removes pipeline-induced reordering but
  serialises hot nodes; deferred until real collectors (Phase 3) show whether order matters for
  features (Phase 4).
- Exact duplicate detection with an unbounded set per node: unbounded memory.

## Consequences
- `IngestEvent` carries a sequence number and a `steady_clock` submit time for latency.
- Sinks must be thread-safe across workers and must not block forever without watching `stop`.
