# FaultTwin AI

**AI-Powered Distributed Database Failure Prediction and Recovery Engine** — an experimental
reliability-engineering platform, written primarily in C++20, that will collect PostgreSQL cluster
telemetry, detect anomalies, predict developing failures, simulate recovery strategies on a custom
discrete-event digital twin and recommend evidence-based actions. It is advisory: it does not
execute destructive or production recovery actions.

> **Status: Phase 2 of 12 (concurrent telemetry ingestion) submitted for review.** What exists
> today is the build system, the telemetry value model with validation, a synthetic-data CLI, a
> bounded concurrent ingest pipeline with a single-threaded baseline, tests and Google Benchmark
> benchmarks. Benchmark numbers so far come from one cloud container and are not independently
> reproduced. Everything else is design. See [`docs/PROGRESS.md`](docs/PROGRESS.md).

## What works now

- `faulttwin_telemetry` library: bounded `NodeId`, `NodeRole`, `RawTelemetrySample` →
  `validate_sample()` → `TelemetrySample` (valid by construction), with explicit missing-vs-zero
  replication lag and a stale-vs-invalid distinction.
- `faulttwin-synth`: generates deterministic synthetic telemetry, injects known defects, validates
  every sample and checks the validator against the injected ground truth.
- `faulttwin_concurrency`: `BoundedQueue<T>`, a bounded MPMC queue with three overload policies
  (block with timeout, reject newest, drop oldest), `std::stop_token` cancellation, close/drain,
  batched pops and full accounting. Refused or evicted items are handed back, never destroyed.
- `faulttwin_ingest`: `IngestPipeline` (producers call `submit()`, `std::jthread` consumers
  validate, detect duplicates and out-of-order samples, apply policies and call a sink), graceful
  shutdown with a drain deadline, and a single-threaded baseline doing identical work.
- Tests (GoogleTest + CLI tests), clean under ASan+UBSan and TSan with GCC and Clang on Linux,
  including multi-producer/multi-consumer stress tests.
- `faulttwin_bench` (Release preset): Google Benchmark suite for Experiments 1 and 2.

## Build and test

Requirements: CMake ≥ 3.25, Ninja, GCC ≥ 13 or Clang ≥ 18, network access to github.com for the
first configure (GoogleTest is fetched at a pinned commit). Sanitizer builds with Clang on Ubuntu
also need `libclang-rt-18-dev`.

```bash
cmake --preset debug          # or: release, asan-ubsan, tsan
cmake --build --preset debug
ctest --preset debug

# Everything, with full logs saved under results/phase-02/<compiler>/
scripts/verify-presets.sh gcc
scripts/verify-presets.sh clang

# Benchmarks (Release build; writes JSON, CSV and a summary under results/phase-02/benchmarks/)
scripts/run-benchmarks.sh gcc
```

## Try the CLI

```bash
./build/debug/src/cli/faulttwin-synth --nodes 3 --samples-per-node 2 --invalid-fraction 0.5 --seed 3
./build/debug/src/cli/faulttwin-synth --nodes 5 --samples-per-node 1000 --seed 7 \
    --invalid-fraction 0.2 --format jsonl --quiet
./build/debug/src/cli/faulttwin-synth --help
```

Exit status 0 means the validator agreed with the injected ground truth for every sample.

## Repository layout (current)

```
include/faulttwin/telemetry/   Public headers: node id, role, sample, validation, synthetic generator
include/faulttwin/concurrency/ Bounded queue, unbounded reference queue, queue result types
include/faulttwin/ingest/      Order tracker, event processor, pipeline, single-threaded baseline
src/telemetry/                 Library implementation
src/concurrency/, src/ingest/  Library implementations
bench/                         Google Benchmark suite (Release only)
src/cli/                       faulttwin-synth (options + report are a small testable library)
tests/unit/                    GoogleTest suites
tests/cli/                     CLI determinism check (CMake script)
cmake/                         Warnings, sanitizers, pinned dependencies
scripts/verify-presets.sh      Configure/build/test every preset and save logs
scripts/run-benchmarks.sh      Release build + benchmarks + environment record
scripts/summarize-benchmarks.py  JSON -> runs.csv, summary.csv, summary.md (bootstrap CIs)
results/phase-01/, phase-02/   Raw logs and benchmark output per phase
docs/                          Architecture, ADRs, evaluation plan, evidence ledger, phase reports
.github/workflows/ci.yml       Linux CI: gcc/clang × 4 presets, clang-format, clang-tidy
```

Directories for later modules are created only when those modules are implemented.

## Documentation
- [Requirements and MVP boundaries](docs/architecture/requirements.md)
- [Architecture (target design)](docs/architecture/overview.md)
- [Acceptance criteria](docs/architecture/acceptance-criteria.md)
- [Dependencies](docs/architecture/dependencies.md)
- [ADRs](docs/adr/)
- [Metric definitions](docs/evaluation/metric-definitions.md), [baselines](docs/evaluation/baselines.md),
  [protocols](docs/evaluation/experiment-protocols.md), [targets](docs/evaluation/metric-targets.md),
  [verified results](docs/evaluation/verified-results.md),
  [limitations](docs/evaluation/limitations.md)
- [Evidence ledger](docs/resume/evidence-ledger.md)
- [Phase 1 report](docs/phase-reports/phase-01.md), [Phase 2 report](docs/phase-reports/phase-02.md)

## Development process and AI assistance

This project is designed, reviewed and owned by its sole author. An AI coding assistant is used for
implementation help, and every change is reviewed, built and committed by the author. Results are
reported only when measured.

## Licence

Not yet chosen. Third-party components keep their own licences (GoogleTest: BSD-3-Clause;
Google Benchmark: Apache-2.0).
