# FaultTwin AI — Progress Tracker

Phases follow the owner's master specification. Statuses: Not started · In progress ·
In review (awaiting owner approval) · Complete. A phase is **Complete** only after its acceptance
criteria (`docs/architecture/acceptance-criteria.md`) are verified with saved output **and** the owner
approves it after technical review.

| # | Phase | Status | Notes |
|---|---|---|---|
| 0 | Architecture and evaluation design | Complete | Docs in `docs/architecture/`, `docs/adr/`, `docs/evaluation/` |
| 1 | C++20 engineering foundation | Complete | Commits `3223488`, `b36a903`. Linux: GCC + Clang, 4 presets pass. macOS debug passes. CI green (run #1) |
| 2 | Concurrent telemetry ingestion | In review | Commit `3539c5c`. Linux: GCC + Clang, 4 presets pass, TSan clean. macOS debug + release pass (AppleClang 21). CI green (run #3). Benchmarks measured in a cloud container only |
| 3 | PostgreSQL monitoring | Not started | |
| 4 | Time-series features and anomaly detection | Not started | |
| 5 | Persistence, APIs and observability | Not started | |
| 6 | Chaos engineering and dataset generation | Not started | |
| 7 | ML training and native C++ inference | Not started | |
| 8 | Custom digital twin | Not started | |
| 9 | Recovery evaluation and ranking | Not started | |
| 10 | AI explanations and dashboard | Not started | |
| 11 | Performance optimization and hardening | Not started | |
| 12 | Portfolio, documentation and interview preparation | Not started | |

## Phase log

### 2026-11-25 — Phase 1 approved; Phase 2 submitted for review
- The owner approved Phases 0 and 1 and authorised Phase 2 ("next").
- Implemented: `faulttwin_concurrency` (`BoundedQueue<T>` with block / reject-newest /
  drop-oldest, timeouts, `stop_token` cancellation, close/drain, `pop_batch`; `UnboundedQueue<T>`
  reference), `faulttwin_ingest` (`NodeOrderTracker`, `EventProcessor`, `IngestPipeline` with
  `std::jthread` consumers and drain-deadline shutdown, single-threaded baseline), Google Benchmark
  suite and result scripts. ADR-0006 accepted; ADR-0012 and ADR-0013 added.
- Verified (assistant, Linux container): all tests pass in debug, release, asan-ubsan and tsan with
  GCC 13.3 and Clang 18.1; TSan stress suites clean over 20 extra repeats per compiler;
  clang-format and clang-tidy clean. Logs in `results/phase-02/`.
- Measured (assistant, cloud container only): Experiment 1 grid and a reduced Experiment 2.
  With heavy per-event work the pipeline reached up to 3.4× the single-threaded baseline; with
  near-zero work it was 0.1–0.4× (slower). Details in the Phase 2 report §16–17.
- Owner committed and pushed `3539c5c`. On the owner's Mac (AppleClang 21) debug passed 114/114
  and release passed 115/115, including `bench.smoke`.
- Not run: macOS sanitizer presets, full Experiment 2, coverage, independent reproduction.
- CI run #3 on `3539c5c` passed (4m 59s).
- Waiting on: owner approval (criterion 2.15). Phase 3 not started.
- Report: `docs/phase-reports/phase-02.md`.

### 2026-11-25 — Phases 0 and 1 submitted for review
- Implemented: telemetry value model, validation contracts, freshness classification, deterministic
  synthetic generator, `faulttwin-synth` CLI, CMake presets, sanitizer builds, CI workflow, docs.
- Verified: 57/57 CTest tests pass for GCC 13.3 and Clang 18.1 in debug, release, asan-ubsan and
  tsan on Linux x86_64 (logs in `results/phase-01/`). clang-format and clang-tidy clean.
- Committed by the owner as root commit `3223488` (75 files). `ci.yml` not yet committed.
- Owner ran the macOS debug preset: AppleClang 21, 57/57 pass. Two warnings, both outside our own
  code: GoogleTest `-Wcharacter-conversion`, and an ld duplicate-library warning, now fixed
  (uncommitted).
- Owner committed and pushed `b36a903` (workflow, link fix, macOS log). CI run #1 on it passed: all
  10 jobs, 4m 45s (https://github.com/lavanesh-tech/faulttwin-ai/actions/runs/45714766090).
- Not verified: macOS release/sanitizer presets, line coverage.
- Waiting on: owner review and approval (criterion 1.17). Phase 2 not started.
- Report: `docs/phase-reports/phase-01.md`.
