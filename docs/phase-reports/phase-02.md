# Phase 2 Implementation Report — Concurrent Telemetry Ingestion

Date: 2026-11-25 · Prepared for independent technical review · Status: **In review, not complete.**

Terminology: *implemented* = code exists; *tested* = automated tests ran and passed;
*measured* = a metric was recorded under a protocol; *independently verified* = someone other than
the implementer reproduced it. In Phase 2 some metrics are **measured, in the assistant's cloud
container only**. Nothing is independently verified.

Base: the owner's `main` at `d65152f` (CI green). All Phase 2 changes are uncommitted in the
owner's working tree. The assistant ran no git command that changes anything.

---

## 1. Phase objective
Build the first concurrent subsystem: a bounded thread-safe queue with explicit overload policies,
producers and consumers, backpressure, graceful shutdown, cancellation and timeouts, duplicate /
invalid / out-of-order policies, concurrency tests run under ThreadSanitizer, a single-threaded
baseline, and initial Google Benchmark benchmarks (Release, machine-readable output, environment
metadata). Every event is counted as attempted, accepted, processed, rejected or dropped, and no
work is discarded silently. No PostgreSQL, Drogon, gRPC, ML or other later-phase technology.

## 2. Business problem addressed
A monitoring system that falls behind or silently loses telemetry under load gives operators a
wrong picture exactly when the database is in trouble. Phase 2 makes overload explicit: the queue
is bounded (memory cannot grow without limit), the overload behaviour is a named policy, and every
refused, evicted or unfinished event is counted and handed back to its producer. It also measures,
honestly, when concurrency helps and when it costs more than it gives. No business metric is
improved yet; there is no real collector until Phase 3.

## 3. Architecture decisions
- **ADR-0006 (now Accepted)** `BoundedQueue<T>`: ring buffer + one mutex + two
  `condition_variable_any`; policies `kBlock` / `kRejectNewest` / `kDropOldest`; refused and
  evicted items handed back in `PushResult`; `steady_clock` deadlines; `std::stop_token`
  cancellation; `close()` drains. Wake-up policy with waiter counts, edge-triggered and low-water
  notifies, cascade, and `pop_batch`. A `QueueLike` concept is shared with the unbounded reference.
- **ADR-0012 (new)** Ingest semantics: one `EventProcessor` shared by pipeline and baseline; eight
  `Disposition`s, each event delivered to the sink exactly once (rejections are the dead-letter
  path); ordering identity (node, `collected_at`) with a bounded per-node window; defaults reject
  duplicates and accept (but flag) late data; ownership hand-back from `submit()`; shutdown with a
  drain deadline then `request_stop()`; the counter invariants.
- **ADR-0013 (new)** Google Benchmark v1.9.4 pinned by commit, Release preset only, raw JSON plus
  generated CSV/Markdown summaries with bootstrap CIs.
- **Phase 1 open questions (§23 there), defaults chosen and documented:**
  1. `kUnknown` role stays valid (lag forbidden). Unchanged: nothing in Phase 2 depends on it.
  2. Lag on a primary, even 0, stays invalid. Unchanged.
  3. 5 s future-skew default kept; revisit with measured collector clock drift in Phase 3.
  4. One issue per field kept. The pipeline passes the full issue list to the sink, so it can be
     widened later without changing the pipeline.
  5. No `src/core` yet: Phase 2 needed only an injectable "now" (`NowFn`, a `std::function`) and
     `steady_clock` for timeouts. Two new modules were created instead, `concurrency` and `ingest`,
     matching `overview.md` §4.
  6. macOS is **not** added to CI in this phase; a proposal is in §22. The owner can add it.
- No CLI for the pipeline was added: the tests and the benchmark binary exercise it end to end, and
  a CLI would mostly duplicate option parsing. Phase 3's collector daemon is the natural front end.

## 4. Files created and modified
New:

| Path | Purpose |
|---|---|
| `include/faulttwin/concurrency/queue_types.hpp` | Policies, push/pop statuses and results, `QueueStats`, `QueueLike` concept |
| `include/faulttwin/concurrency/bounded_queue.hpp` | `BoundedQueue<T>` |
| `include/faulttwin/concurrency/unbounded_queue.hpp` | `UnboundedQueue<T>` (Experiment 2 reference design only) |
| `src/concurrency/{CMakeLists.txt,queue_types.cpp}` | `faulttwin_concurrency` library |
| `include/faulttwin/ingest/{order_tracker,event_processor,pipeline,baseline}.hpp` | Ingest public API |
| `src/ingest/{CMakeLists.txt,order_tracker.cpp,event_processor.cpp,pipeline.cpp,baseline.cpp}` | `faulttwin_ingest` library |
| `tests/unit/{bounded_queue,queue_stress,order_tracker,pipeline}_test.cpp`, `tests/unit/ingest_test_support.hpp` | Tests |
| `bench/{CMakeLists.txt,ingest_bench.cpp,bench_support.hpp}` | Benchmarks + `bench.smoke` test |
| `cmake/BenchmarkDependency.cmake` | Pinned Google Benchmark |
| `scripts/run-benchmarks.sh`, `scripts/summarize-benchmarks.py` | Benchmark run + summary |
| `docs/adr/0012-ingest-pipeline-semantics.md`, `docs/adr/0013-google-benchmark.md` | ADRs |
| `docs/phase-reports/phase-02.md` | This report |
| `results/phase-02/**` | Raw evidence |
| `ci.phase-02.yml` (repository root) | Updated CI workflow for the owner to move into `.github/workflows/ci.yml` |

Modified: `CMakeLists.txt` (benchmark option), `CMakePresets.json` (release enables benchmarks),
`src/CMakeLists.txt`, `tests/CMakeLists.txt`, `scripts/verify-presets.sh` (results go to
`results/phase-02/` by default; empty-array fix for macOS bash 3.2), `README.md`,
`docs/PROGRESS.md`, `docs/adr/0006-queue-implementation-strategy.md`,
`docs/architecture/{overview,dependencies,acceptance-criteria}.md`,
`docs/evaluation/{metric-definitions,baselines,experiment-protocols,limitations,verified-results}.md`,
`docs/resume/evidence-ledger.md`.

Size: about 3,100 new lines of C++ (headers, sources, tests, benchmarks) and 340 lines of scripts.

## 5. Dependencies introduced and justification
| Dependency | Pin | Why |
|---|---|---|
| Google Benchmark | v1.9.4, commit `eddb0241389718a23a42db6af5f0164b6e0139af` | Required by the specification; benchmark-only, Release preset only, never linked into product targets. Apache-2.0. |
| `Threads::Threads` | system | `std::jthread` |
| Python 3 standard library | 3.13 in the container | Summarising results only; not needed to build or test |

Not introduced (specification constraint): PostgreSQL/libpq, Drogon, gRPC/Protobuf, ONNX Runtime,
Boost, any lock-free library.

## 6. Algorithms and data structures
- **BoundedQueue:** fixed `std::vector<std::optional<T>>` ring (head index + size), one mutex, two
  condition variables. Waiter counts kept under the lock let notifiers skip the notify system call
  when nobody waits. Consumers are woken on the empty → non-empty edge; producers when the depth
  falls to half the capacity (hysteresis); a woken thread that leaves work behind wakes one more
  (cascade). These keep the invariant "if items exist and consumers wait, one is awake or
  signalled" (and the same for free slots and producers), so no wake-up is lost. `pop_batch` moves
  up to N items per lock acquisition. Drop-oldest evicts the head and returns it.
- **UnboundedQueue:** `std::deque` + mutex + one condition variable. Reference only.
- **NodeOrderTracker:** 16 lock stripes, each an `unordered_map<string, NodeState>`; per node the
  newest `collected_at` and a 64-entry ring of recent times (linear scan). A global atomic node
  count with a CAS loop enforces `max_nodes` (1,024) without a global lock.
- **EventProcessor:** validate (Phase 1 code) → tracker verdict → policy → sink, inside one
  `noexcept` function. Counters: one cache-line-aligned array of 8 relaxed atomics per worker,
  summed on read.
- **IngestPipeline:** producers call `submit()`, which stamps a sequence number (atomic
  fetch_add) and a `steady_clock` time and pushes. K `std::jthread` consumers loop on
  `pop_batch(64)`. Shutdown as in ADR-0012.
- **Benchmark statistics:** nearest-rank percentiles per run; across runs median, mean, sample
  standard deviation and a 95% percentile bootstrap CI of the mean (10,000 resamples, fixed seed).

## 7. Time and space complexity
| Operation | Time | Space |
|---|---|---|
| `push` / `try_push` / `pop` / `try_pop` | O(1) under the lock (plus waiting) | O(capacity) allocated once |
| `pop_batch(n)` | O(n) under one lock | appends to the caller's vector |
| `take_all` | O(depth) | O(depth) result |
| `NodeOrderTracker::observe` | O(L) hash + O(W) ring scan, W = 64 | O(N·W) timestamps, N ≤ 1,024 nodes |
| `EventProcessor::process` | O(L + W) | O(1) beyond validation |
| Pipeline memory | — | capacity × sizeof(`optional<IngestEvent>`) + consumers × batch events + tracker; bounded |
| `UnboundedQueue` | O(1) amortised | **unbounded** (that is its purpose) |

## 8. Memory ownership decisions
- Queues own their items by value. A refused or evicted item is moved back to the caller inside
  `PushResult` / `SubmitResult`; the queue never destroys an item it was given, except by the
  caller's choice after it is returned.
- `T` must be nothrow-move-constructible (`static_assert`), so moves inside the lock cannot throw;
  `pop_batch` reserves the output before removing anything.
- `ProcessedEvent` is a non-owning view (pointers/span valid only during the sink call).
- Per-worker counters and tracker stripes live in `unique_ptr<T[]>` because atomics and mutexes
  are not movable. No raw owning pointers.
- `ShutdownReport::abandoned` returns ownership of unprocessed samples to the caller.

## 9. Thread-safety considerations
- All queue state, including statistics, is guarded by one mutex; `stats()` is a consistent
  snapshot. Notifies happen after unlocking, decided from state read under the lock.
- Pipeline counters are relaxed atomics; `counters()` reads processed and abandoned *before* the
  queue's `removed`, so a snapshot taken while running never shows negative in-flight work. After
  `shutdown()` the snapshot is exact.
- The sink is called concurrently with distinct worker indices; per-worker state needs no lock.
- `shutdown()` is serialised by its own mutex and idempotent. Lock order is never nested across
  queue, tracker stripe, exit and unprocessed mutexes, so there is no lock-order cycle.
- No exception escapes a thread: `process()` is `noexcept`; the consumer loop has a final
  catch-all.
- Verified with ThreadSanitizer (GCC and Clang) on every test, including 4×4 MPMC stress tests and
  repeated runs (§15). TSan only reports races in interleavings that actually happened.

## 10. Failure scenarios handled
Queue full under each policy; producer timeout; producer cancellation (before and during the
wait); consumer timeout and cancellation on an empty queue; close while producers and consumers
wait; push after close; drain after close; capacity 0 and other invalid configuration
(`std::invalid_argument`); move-only items; invalid samples (validation issues delivered to the
sink); exact duplicates; out-of-order samples; duplicates older than the window; too many distinct
nodes; a sink that throws (counted `failed`, consumer continues); a sink that blocks (shutdown
deadline, cancellation through the sink's `stop_token`, batch remainder and queue contents handed
back); shutdown called twice; destruction without shutdown; thread-creation failure in the
constructor (already-started consumers are stopped and joined before rethrowing; not tested, it
needs fault injection).

## 11. Security considerations
- Memory is bounded under overload (bounded queue, bounded batches, bounded tracker: ≤ 1,024 nodes
  × 64 timestamps), so a flood of events or invented node ids cannot exhaust memory through the
  ingest path. The unbounded queue exists only as a benchmark baseline.
- Input is still untrusted and validated by the Phase 1 code before anything else uses it.
- Overload is visible (counted and returned), which matters for detecting a telemetry flood.
- No network, secrets or new I/O. Benchmarks read `/proc/self/status` only.

## 12. Actual build commands
Run by the assistant in an ephemeral cloud container: Linux 6.18 x86_64, Intel Xeon @ 2.10 GHz,
4 vCPUs, 15.7 GiB RAM, Ubuntu 24.04 toolchain (GCC 13.3.0, Clang 18.1.3, CMake 3.28.3, Ninja 1.11.1),
`libclang-rt-18-dev` installed with apt for Clang sanitizers. No git repository was present there.

```bash
scripts/verify-presets.sh gcc
scripts/verify-presets.sh clang
scripts/run-benchmarks.sh gcc
scripts/run-benchmarks.sh clang
```

`verify-presets.sh` runs, per preset P: `cmake --preset P -B build/P-<compiler>
-DCMAKE_C_COMPILER=… -DCMAKE_CXX_COMPILER=…`, `cmake --build build/P-<compiler>`, then ctest.
`run-benchmarks.sh` builds `build/release-<compiler>` (target `faulttwin_bench`), writes
`environment.json`, runs the benchmark binary with `--benchmark_out=… --benchmark_out_format=json`,
and calls the summariser.

## 13. Actual test commands
```bash
ctest --test-dir build/<preset>-<compiler> --output-on-failure --no-tests=error
TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 build/tsan-<compiler>/tests/faulttwin_unit_tests \
  --gtest_filter='QueueStress*:Pipeline*:OrderTracker*:BoundedQueue*:UnboundedQueue*:Policies/*:BaselineTest*' \
  --gtest_repeat=20 --gtest_brief=1
find include src tests bench -name '*.cpp' -o -name '*.hpp' | xargs clang-format --dry-run --Werror
find src -name '*.cpp' | xargs clang-tidy -p build/tidy --quiet --warnings-as-errors='*'
```
`build/tidy` is a Clang debug configure with `FAULTTWIN_BUILD_BENCHMARKS=ON`.

## 14. Test results
| Platform / compiler | debug | release | asan-ubsan | tsan | Run by |
|---|---|---|---|---|---|
| Linux x86_64 / GCC 13.3.0 | 114/114 pass | 115/115 pass | 114/114 pass | 114/114 pass | assistant (cloud container) |
| Linux x86_64 / Clang 18.1.3 | 114/114 pass | 115/115 pass | 114/114 pass | 114/114 pass | assistant (cloud container) |
| macOS arm64 / AppleClang | NOT RUN | NOT RUN | NOT RUN | NOT RUN | — |

114 CTest tests = the 57 Phase 1 tests (unchanged) + 57 new GoogleTest cases: BoundedQueueTest
20, UnboundedQueueTest 2, QueueStressTest 6, OrderTrackerTest 8, PipelineTest 17,
Policies/PipelinePolicyTest 3 (parameterised duplicate/out-of-order policy combinations),
BaselineTest 1. The release preset adds `bench.smoke` (115). Timing assertions only check lower
bounds (a 30 ms timeout waited at least 30 ms), with a 10 s upper limit for "wakes up promptly",
so slow sanitizer builds and loaded machines do not make them flaky.

- clang-format 18.1.3 over `include src tests bench`: clean.
- clang-tidy 18.1.3 on `src/` with the committed config, warnings as errors: exit 0. Findings
  fixed along the way: nine `misc-const-correctness` (locks declared `const`), one
  `bugprone-unchecked-optional-access`, one `readability-convert-member-functions-to-static`.
- Coverage: **NOT RUN**.

## 15. Sanitizer results
- ASan+UBSan: 114/114 with GCC and Clang, no reports (logs in `results/phase-02/*/asan-ubsan.log`).
- TSan: 114/114 with GCC and Clang, no reports (`results/phase-02/*/tsan.log`). The kernel in the
  container already had `vm.mmap_rnd_bits=28`.
- TSan repeat: the 57 Phase 2 test cases were run 20 more times per compiler under TSan
  (`--gtest_repeat=20`): 20/20 iterations passed with both compilers, no reports
  (`results/phase-02/tsan-repeat/`). TSan reports only races in interleavings that occurred.
- During development, TSan runs of earlier versions of the code were also clean; no sanitizer
  finding had to be fixed in Phase 2.

## 16. Benchmark results
**Measured by the assistant in a cloud container, not on the owner's machine, not independently
reproduced.** Environment (`results/phase-02/benchmarks/<compiler>/environment.json`): Linux 6.18
x86_64, Intel Xeon @ 2.10 GHz, 4 logical CPUs, 15.7 GiB RAM, no cgroup CPU or memory limit
visible, shared host; Release (`-O3 -DNDEBUG`); Google Benchmark v1.9.4; CPU frequency scaling
reported as disabled; load average 3.2 (GCC run) and 2.7 (Clang run) at start. Code: `d65152f`
plus uncommitted Phase 2 changes. Every configuration: 1,000,000 events (64 nodes × 15,625
ticks, seed 20261125, 1% injected defects), 1 untimed 100k-event warm-up then 1 timed pass, 5
repetitions; values are medians, CIs are 95% bootstrap CIs of the mean. Every one of the 310
runs per compiler passed its accounting invariant (`invariant_ok = 1`). Full tables:
`results/phase-02/benchmarks/{gcc,clang}/summary.md`.

`work` is extra CPU work per event inside the sink (SplitMix64 iterations), identical in the
baseline and the pipeline: 0 = validation and ordering only (about 0.2 µs per event);
4096 ≈ 4.3 µs (GCC) / 5.7 µs (Clang) per event. Clang's build of this loop is slower, so compare
designs within one compiler, never across.

**Single-threaded baseline**

| work | GCC events/s | Clang events/s |
|---|---|---|
| 0 | 4.68M (CI 4.19M–4.73M) | 5.13M (4.34M–5.54M) |
| 4096 | 233.0k (232.4k–241.1k) | 176.2k (171.5k–185.5k) |

**Pipeline (block policy, batch 64), selected rows** (all 48 rows per compiler in `summary.md`):

| P × C | capacity | work | GCC events/s | vs baseline | GCC p99 | Clang events/s | vs baseline | Clang p99 |
|---|---|---|---|---|---|---|---|---|
| 1 × 1 | 1024 | 0 | 1.11M | 0.24× | 0.51 ms | 1.09M | 0.21× | 0.50 ms |
| 4 × 1 | 16384 | 0 | 1.95M | 0.42× (best light-work row) | 6.5 ms | 1.86M | 0.36× | 5.8 ms |
| 1 × 4 | 1024 | 0 | 440k | 0.09× | 0.46 ms | 507k | 0.10× | 0.48 ms |
| 1 × 1 | 1024 | 4096 | 229k | 0.98× | 6.6 ms | 168k | 0.95× | 7.5 ms |
| 1 × 2 | 1024 | 4096 | 422k | 1.81× | 3.3 ms | 315k | 1.79× | 4.1 ms |
| 1 × 4 | 1024 | 4096 | 772k | 3.31× | 1.76 ms | 593k | 3.36× | 2.2 ms |
| 2 × 4 | 16384 | 4096 | 800k | 3.43× | 21.7 ms | 589k | 3.34× | 29.0 ms |
| 2 × 8 | 16384 | 4096 | 815k | 3.50× (best GCC row) | 21.9 ms | 592k | 3.36× | 32.5 ms |
| 1 × 8 | 16384 | 4096 | 799k | 3.43× | 22.9 ms | 609k | 3.46× (best Clang row) | 29.5 ms |

Ranges over all 24 configurations per work level: light work 0.09×–0.42× (GCC) and
0.10×–0.42× (Clang) of the baseline; heavy work 0.94×–3.50× (GCC) and 0.94×–3.46× (Clang).
p99 latency: capacity 1024: 0.35–0.62 ms (light) and 1.8–6.7 ms (heavy, GCC); capacity 16384:
up to 84 ms (GCC) / 103 ms (Clang) with heavy work, because the block policy keeps a 16k-deep
queue full and each event waits behind all the others (Little's law).

**Batch ablation** (capacity 1024), events/s, batch 1 → batch 64:

| P × C, work | GCC | Clang |
|---|---|---|
| 1 × 1, 0 | 1.40M → 1.19M (−15%) | 1.26M → 1.10M (−13%) |
| 2 × 4, 0 | 960k → 720k (−25%) | 868k → 760k (−12%) |
| 2 × 4, 4096 | 503k → 763k (+52%) | 453k → 583k (+29%) |

Batching helps only with heavy work in the final design. With light work it hurts. A plausible
explanation (not verified with a profiler): one consumer takes a whole batch, the others find the
queue empty and go to sleep, and more sleep/wake cycles follow. The default stays 64 because
realistic ingest work (Phase 3+) is closer to the heavy case; see §23.

**Experiment 2 (reduced): one producer, one slow consumer (~2.3 µs/event), 200,000 events**

| design | GCC wall ms | processed | rejected | dropped (handed back) | queue high-water | peak RSS MiB |
|---|---|---|---|---|---|---|
| bounded, block, cap 1024 | 407 | 200,000 | 0 | 0 | 1,024 | 39.5 |
| bounded, reject-newest | 26 | 6,941 | 193,059 | 0 | 1,024 | 39.5 |
| bounded, drop-oldest | 47 | 9,271 | 0 | 190,729 | 1,024 | 39.5 |
| unbounded reference | 452 | 200,000 | 0 | 0 | 188,000 | 43.3 |

Clang: block 572 ms, unbounded 602 ms, high-water 1,024 vs 192k, RSS 39.4 vs 43.2 MiB. The peak
RSS gap is small because the 200k-event input buffer dominates the process's memory; queue depth
is the clearer signal. In every run `processed + handed back = 200,000`.

**Queue micro-benchmark** (uncontended `try_push` + `try_pop` pair, `uint64_t`): bounded 29.5M
pairs/s (GCC) / 26.6M (Clang); unbounded 24.5M / 28.9M.

## 17. Baseline comparison
- **Experiment 1 question: "does concurrency increase useful processed events under equivalent
  work?"** It depends on the work per event. With about 0.2 µs of work, **no**: every
  configuration was slower than one thread (0.09×–0.42×). With about 4–6 µs of work, **yes**:
  up to 3.50× (GCC) / 3.46× (Clang) on 4 vCPUs, with 1 consumer roughly equal to the baseline
  (0.94×–1.04×), and scaling efficiency (ING-SCALE) 0.83–0.97 with 2 consumers and 0.73–0.88 with
  4 (both compilers). 8 consumers on 4 vCPUs add almost nothing (efficiency 0.37–0.45).
- Relative values are always shown with the absolute baseline (233.0k and 4.68M events/s, GCC).
- Targets from `metric-targets.md`: ≥ 2× throughput over the baseline: **met for heavy work,
  not met for light work**. ≥ 10,000 events/s sustained: met in every configuration. p99 < 20 ms:
  met at capacity 1024 in every configuration; **not met** at capacity 16384 with heavy work.
  ≥ 30% peak memory reduction vs the reference: **not met as measured by RSS** (−8.8%) in the
  reduced Experiment 2, although the queue itself held 99.5% fewer events.
- **How the design got here (exploratory runs, GCC, 5 repetitions, console output only, not
  saved as artifacts, so not evidence):** P1×C1 / P1×C4, capacity 1024, work 0, events/s:
  first version notifying on every push and pop 615k / 198k; skip notify without waiters
  738k / 204k; plus `pop_batch(64)` 1.04M / 86k; plus edge-triggered consumer wake and cascade
  1.22M / 484k; plus low-water producer wake ≈ 1.18M / 458k, and the reduced Experiment 2 block
  run went from 929 ms to 403 ms. The baseline in those sessions was about 4.2M events/s. These
  numbers explain the decisions in ADR-0006; only the recorded runs above count as results.

## 18. Raw evidence artifact locations
- `results/phase-02/{gcc,clang}/`: `environment.txt` and full configure/build/test logs for each
  preset.
- `results/phase-02/tsan-repeat/{gcc,clang}.log`: 20 extra TSan iterations of the Phase 2 suites.
- `results/phase-02/benchmarks/{gcc,clang}/`: `environment.json`, raw Google Benchmark JSON
  (`ingest.json`, `overload-*.json`), console logs, `runs.csv`, `summary.csv`, `summary.md`.
- All produced by the assistant in the container described in §12 and §16. One hand edit, the
  repaired `git_uncommitted_files` field, is described in `results/phase-02/README.md`.

## 19. Updated evidence ledger
`docs/resume/evidence-ledger.md`: ING-TPUT/REL-THR (heavy and light work, both rows),
ING-LAT-p99, ING-MEM/REL-MEM and ING-SUCC are `MEASURED (container only)`, with commit, artifact
and limitations. The ledger says these rows should not go on a resume until the owner reproduces
them on a known commit. `docs/evaluation/verified-results.md` lists the same values for both
compilers, none independently verified. A Phase 2 functional table records the TESTED items.

## 20. Git status and commit SHA
- Base: `d65152f` on `main` (the owner's commit, CI green). The assistant ran **no** git command
  that changes anything: no commit, push, branch, stash or config change. In the owner's clone,
  `git status` was clean before the Phase 2 files were copied in.
- All Phase 2 files are uncommitted changes in the owner's working tree, for the owner to review
  and commit. The assistant's file tool cannot write into `.github/`, so the CI change is at the
  repository root as `ci.phase-02.yml` (only change: log paths `results/phase-01/` →
  `results/phase-02/`), for the owner to move.
- No file names Claude, Anthropic or any AI as author or co-author.

Suggested commit message (for the owner to review and commit):
```
Phase 2: concurrent telemetry ingestion

- concurrency: BoundedQueue<T> (block / reject-newest / drop-oldest,
  steady_clock deadlines, stop_token cancellation, close/drain,
  pop_batch, waiter-aware wake-ups) and an UnboundedQueue reference
- ingest: NodeOrderTracker (duplicates, out-of-order, node cap),
  EventProcessor, IngestPipeline (jthread consumers, drain-deadline
  shutdown, refused/abandoned samples handed back), single-threaded
  baseline
- 57 new tests incl. MPMC stress tests; all presets pass with GCC 13
  and Clang 18, TSan and ASan+UBSan clean
- Google Benchmark v1.9.4 (pinned), bench suite, run/summary scripts,
  results under results/phase-02/
- ADR-0006 accepted, ADR-0012, ADR-0013; docs, ledger, tracker updated
- CI log paths moved to results/phase-02/
```

## 21. Known bugs and limitations
- **With cheap per-event work the pipeline is slower than one thread** (§16–17). The mutex
  hand-off, wake-ups and moving each ~120-byte event between cores cost more than validating it.
  This is a property of the design at this workload, measured, not a hidden bug. Phase 11 may
  revisit (lock-free queue, per-producer batching, spin-then-park), only if measured to matter.
- All numbers are from one shared 4-vCPU cloud container. Configurations with more threads than
  vCPUs (e.g. 4 producers + 8 consumers) are oversubscribed and noisy.
- Experiment 2 is reduced (one arrival profile, no memory cap, no latency or drain-time numbers).
- Out-of-order counts include reordering caused by several consumers; there is no per-node
  consumer affinity yet.
- Duplicates older than the 64-entry per-node window are classified as out-of-order.
- Cancellation is cooperative: a sink that blocks without watching its `stop_token` can stall
  `shutdown()`.
- No fairness among blocked producers; a producer with a deadline can time out while others
  succeed.
- The thread-creation failure path in the `IngestPipeline` constructor is not tested.
- clang-tidy was run on `src/` only, as in Phase 1; tests and benchmarks were not linted.
- macOS has not built Phase 2. If the owner's libc++ lacks `std::jthread` or
  `condition_variable_any::wait(lock, stop_token, pred)`, the build fails there.
- The CI workflow change (`ci.phase-02.yml`) has not run on GitHub.
- Coverage is not measured.

## 22. Incomplete requirements
| Requirement | Status |
|---|---|
| Experiment 1 full grid, 5 repetitions, Release, GCC and Clang | Done in the cloud container (see §16) |
| Experiment 1: ING-CPU, ING-MEM, ING-SAT | NOT RUN (CPU time is in the raw JSON but not summarised) |
| Experiment 2 full protocol (2×/5×/10× profiles, 60 s, memory cap, drain time) | NOT RUN; reduced version only |
| Owner-machine build and test (macOS) | NOT RUN |
| CI run of the Phase 2 code | NOT RUN (needs the owner's commit and the moved workflow) |
| Independent reproduction of any benchmark | NOT RUN |
| Coverage | NOT RUN |
| macOS in CI (Phase 1 question 6) | Not added. Proposal: a `macos-14` job running the debug preset only, added by the owner if wanted |

## 23. Questions requiring architecture review
1. Is rejecting duplicates and accepting out-of-order samples the right default, or should late
   samples beyond some age be rejected?
2. Should consumers be sharded by node (per-node order preserved, hot nodes serialised) before
   Phase 4 features depend on order?
3. Is a 64-sample duplicate window and a 1,024-node cap right for the target topology, or should
   both come from configuration in Phase 3?
4. Is "hand the sample back to the producer" the right contract for refusals, or should the
   pipeline own a dead-letter store (Phase 5 persistence)?
5. Is the 5 s default destructor drain timeout acceptable, or should destroying an un-shut-down
   pipeline be treated as a programming error?
6. Should the full Experiment 2 protocol (with a memory cap, which needs a cgroup or container
   limit) be run before Phase 3, or deferred to Phase 11?
7. Should Python 3 be an accepted tooling dependency for result summaries (it will be needed for
   Phase 7 anyway)?
8. Should the consumer batch default stay 64? It measured 29–52% faster with heavy work and
   12–25% slower with light work (§16).

## 24. Practical learning exercises
1. Delete the waiter counts and the edge-triggered notifies in `BoundedQueue` (notify on every
   push and pop) and re-run `BM_OverloadBounded/policy:0`. Explain the difference using system
   time (`time` or `perf stat`).
2. Set `consumer_batch = 1` and compare `BM_PipelineBatchAblation` rows. Why does batching help
   most when consumers are faster than producers?
3. Change the low-water mark from capacity/2 to capacity-1 and find the benchmark where it hurts.
4. Remove the cascade notify from `notify_after_pop` and write a test with two consumers using
   plain `pop()` that hangs. What invariant did you break?
5. Make `EventProcessor` counters a single shared `std::atomic` per disposition and measure the
   8-consumer configurations. Look up "false sharing".
6. Break the queue on purpose (read `size_` outside the lock in `stats()`) and run the TSan preset.
   Read the report and match it to the code.
7. Write a sink that sleeps without watching `stop` and call `shutdown(10ms)`. What happens, and
   what would you change?

## 25. Interview questions with explanations
1. **Why a bounded queue?** An unbounded queue turns overload into memory growth and ever-growing
   latency, then an OOM kill: the failure moves somewhere harder to see. A bound forces an explicit
   decision (block, reject, drop) at the point where the overload happens. In the reduced
   Experiment 2 run the unbounded queue held about 185,000 events while the bounded one never held
   more than 1,024.
2. **Block, reject-newest, drop-oldest: when each?** Block gives backpressure to producers that can
   slow down (lossless, latency rises). Reject-newest protects what is already queued and tells the
   producer at once (it can retry or shed). Drop-oldest favours freshness, which suits telemetry
   where a newer sample supersedes an older one; the evicted sample is still counted and returned.
3. **How does the queue avoid losing a wake-up when it skips notifies?** Waiters increment a counter
   under the mutex before the wait releases it; a notifier reads the counter under the same mutex.
   Edge-triggered notifies plus the cascade rule keep "work exists and someone waits ⇒ someone is
   awake". The TSan stress tests and the exactly-once tests exercise this.
4. **Why `condition_variable_any` rather than `condition_variable`?** Only the `_any` variant has
   C++20 waits that take a `std::stop_token`, so cancellation wakes a waiting thread without a
   separate mechanism. The cost is an extra internal mutex.
5. **Why can the concurrent pipeline be slower than a single thread?** When the work per event is
   about 0.2 µs, a lock hand-off, a possible futex wake-up and moving the event's cache lines to
   another core cost more than the work. Concurrency pays when per-event work is large compared
   with the hand-off cost, which the `work=4096` rows show.
6. **What is graceful shutdown here, exactly?** Close admission, let consumers drain until a
   deadline, then request stop; each consumer finishes the event in hand; everything left (batch
   remainders and queue) is handed back and counted. Nothing is destroyed unaccounted.
7. **Why steady_clock for timeouts?** system_clock can jump (NTP, manual change), which would make
   a timeout fire early or never. steady_clock is monotonic.
8. **Why per-worker counters on separate cache lines?** A shared atomic counter is written by every
   consumer for every event; the cache line bounces between cores (false sharing if unrelated
   counters share a line). Per-worker lines are written by one core and summed rarely.
9. **What does TSan prove?** That no data race happened in the interleavings the tests executed.
   It does not prove the absence of races in other interleavings, which is why stress tests are
   repeated and the design is kept simple (one mutex).
10. **How do you make a benchmark comparison fair?** Same work per event in both designs (shared
    `EventProcessor`), same input, Release build, warm-up, repetitions, medians with CIs, and every
    run checks its own accounting invariant so a "fast" run that lost events is rejected.

## 26. Phase acceptance criteria and current status
From `docs/architecture/acceptance-criteria.md`:

| # | Criterion | Status |
|---|---|---|
| 2.1 | Bounded queue with block-with-timeout, reject-newest, drop-oldest | ✅ implemented, tested |
| 2.2 | Attempted/accepted/processed/rejected/dropped counters; nothing discarded silently | ✅ tested; invariant checked in all 620 benchmark runs |
| 2.3 | Producers/consumers with `std::jthread` and `std::stop_token` | ✅ tested |
| 2.4 | Graceful shutdown with a drain deadline | ✅ tested |
| 2.5 | Cancellation and timeout tests | ✅ tested |
| 2.6 | Duplicate, invalid and out-of-order policies with tests | ✅ tested |
| 2.7 | Concurrency stress tests pass under TSan | ✅ GCC + Clang, plus 20 repeats each |
| 2.8 | All presets pass with GCC and Clang (Linux) | ✅ 8/8 |
| 2.9 | Single-threaded baseline with identical work | ✅ tested |
| 2.10 | Release benchmarks per Experiment 1, machine-readable, environment metadata *(T)* | ✅ measured in the cloud container; deviations recorded |
| 2.11 | Experiment 2 *(T)* | ⏳ reduced version only |
| 2.12 | Builds and tests on the owner's macOS machine | ⏳ NOT RUN |
| 2.13 | CI green with Phase 2 code | ⏳ NOT RUN |
| 2.14 | No AI authorship; no assistant commits | ✅ |
| 2.15 | Owner approval after independent review | ⏳ |

**Phase 2 is not complete.** Open: your macOS run (2.12), the CI run after you commit and move
the workflow (2.13), a decision on whether the reduced Experiment 2 is enough for now (2.11), and
your review and approval (2.15). Phase 3 has not been started.

