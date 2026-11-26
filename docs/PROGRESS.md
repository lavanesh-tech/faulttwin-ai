# FaultTwin AI — Progress Tracker

Phases follow the owner's master specification. Statuses: Not started · In progress ·
In review (awaiting owner approval) · Complete. A phase is **Complete** only after its acceptance
criteria (`docs/architecture/acceptance-criteria.md`) are verified with saved output **and** the owner
approves it after technical review.

| # | Phase | Status | Notes |
|---|---|---|---|
| 0 | Architecture and evaluation design | Complete | Docs in `docs/architecture/`, `docs/adr/`, `docs/evaluation/` |
| 1 | C++20 engineering foundation | Complete | Commits `3223488`, `b36a903`. Linux: GCC + Clang, 4 presets pass. macOS debug passes. CI green (run #1) |
| 2 | Concurrent telemetry ingestion | Complete | Commits `3539c5c`, `f0624a4`. Approved by the owner; criterion 2.11 (full Experiment 2) deferred to Phase 11. Linux: GCC + Clang, 4 presets pass, TSan clean. macOS debug + release pass (AppleClang 21). CI green (run #3). Benchmarks measured in a cloud container only |
| 3 | PostgreSQL monitoring | In review | Linux container: GCC + Clang, 4 presets pass, 7/7 integration tests with the lab. macOS (owner): debug 179/179, integration 7/7. CI pending. Collection measured in a cloud container only |
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

### 2026-11-26 — Phase 2 approved; Phase 3 submitted for review
- The owner approved Phase 2 (`f0624a4`, CI green) and authorised Phase 3. Criterion 2.11 (full
  Experiment 2) is deferred to Phase 11.
- Implemented: Compose lab (PostgreSQL 17.12 digest-pinned, primary + 2 async streaming replicas
  with slots), least-privilege `ft_monitor` role, `faulttwin_collectors` (quality model, LSN,
  PostgreSQL snapshots, rates, topology, procfs resources), libpq adapter with client deadlines,
  `faulttwin-monitor` CLI, integration tests, measurement scripts. ADR-0014 and ADR-0015 added.
- Verified (assistant, Linux container): see the Phase 3 report §14–15; 7/7 integration tests pass
  against the lab, including a stopped and a paused replica.
- Measured (assistant, cloud container only): 100 % collection success and coverage; steady cycle
  15.2 ms mean / 20.5 ms p95 for 3 nodes; client CPU 1.7 ms per cycle; pgbench impact within noise.
- Not run: macOS, CI with the new workflow, TLS, independent reproduction, MON-DETLAT experiment.
- Owner ran Phase 3 on the Mac: owner-run on Apple Silicon, AppleClang, libpq 19.0 (Homebrew), Docker 29.6.2 / Compose v5.3.1: debug 179/179 (5 lab tests skipped), integration 7/7, monitor 3 cycles 100 % success. Host CPU/memory/load are `missing` on macOS (no procfs), as designed. Fixed afterwards: a duplicate `libfaulttwin_collectors.a` on the unit-test link line (Apple ld warning).
- Waiting on: CI and approval (criteria 3.14, 3.16). Phase 4 not started.
- Report: `docs/phase-reports/phase-03.md`.

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
