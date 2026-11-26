# FaultTwin AI

**AI-Powered Distributed Database Failure Prediction and Recovery Engine** — an experimental
reliability-engineering platform, written primarily in C++20, that will collect PostgreSQL cluster
telemetry, detect anomalies, predict developing failures, simulate recovery strategies on a custom
discrete-event digital twin and recommend evidence-based actions. It is advisory: it does not
execute destructive or production recovery actions.

> **Status: Phase 3 of 12 (PostgreSQL monitoring) submitted for review.** What exists today is
> the build system, the telemetry value model with validation, a synthetic-data CLI, a bounded
> concurrent ingest pipeline, a Docker Compose PostgreSQL lab (primary + 2 streaming replicas), a
> read-only PostgreSQL and host collector (`faulttwin-monitor`) and tests, including integration
> tests against the lab. Measurements so far come from one cloud container and are not
> independently reproduced. Everything else is design. See [`docs/PROGRESS.md`](docs/PROGRESS.md).

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
- Lab cluster (`compose.yaml`): PostgreSQL 17.12 primary + 2 asynchronous streaming replicas with
  physical slots, a least-privilege read-only monitoring role, secrets in `.env`.
- `faulttwin_collectors`: PostgreSQL collection (availability, role, connections, transaction
  counters, probe and statement latency, WAL position, replication state, byte and time lag,
  replay positions, slots), derived rates and topology, and Linux host resources. Every value
  carries a quality (`ok`, `missing`, `stale`, `invalid`, `not_applicable`) with a reason.
- `faulttwin-monitor` (needs libpq): collects from the lab and prints text or JSON lines.

## Build and test

Requirements: CMake ≥ 3.25, Ninja, GCC ≥ 13 or Clang ≥ 18, network access to github.com for the
first configure (GoogleTest is fetched at a pinned commit). Sanitizer builds with Clang on Ubuntu
also need `libclang-rt-18-dev`. The PostgreSQL collector and its tests need libpq
(`sudo apt-get install libpq-dev` or `brew install libpq`; found automatically on macOS); without
it they are skipped and everything else still builds.

```bash
cmake --preset debug          # or: release, asan-ubsan, tsan
cmake --build --preset debug
ctest --preset debug

# Everything, with full logs saved under results/phase-03/<compiler>/
scripts/verify-presets.sh gcc
scripts/verify-presets.sh clang

# Benchmarks (Release build; writes JSON, CSV and a summary under results/phase-02/benchmarks/)
scripts/run-benchmarks.sh gcc
```

## PostgreSQL lab and monitor

Needs Docker (Docker Desktop on macOS) with Compose v2.

```bash
scripts/lab-init-env.sh                  # creates .env with random passwords (git-ignored)
docker compose up -d --wait              # primary + 2 replicas on 127.0.0.1:55432-55434
set -a; . ./.env; set +a                 # exports FT_MONITOR_* for the monitor and tests
./build/debug/src/monitor/faulttwin-monitor --cycles 3
./build/debug/src/monitor/faulttwin-monitor --cycles 10 --interval-ms 500 --format json
scripts/run-integration-tests.sh build/debug   # FT_INTEGRATION=1 ctest -L integration
docker compose down -v                   # stop and delete the lab data
```

Integration tests that need the cluster are skipped unless `FT_INTEGRATION=1`, so a plain `ctest`
passes without Docker. `scripts/measure-collection.sh` records collection cost and freshness.

## Try the synthetic CLI

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
include/faulttwin/collectors/  Reading quality, LSN, SQL session, PostgreSQL snapshot, rates,
                               topology, Linux resources, libpq adapter
src/telemetry/                 Library implementation
src/concurrency/, src/ingest/  Library implementations
bench/                         Google Benchmark suite (Release only)
src/collectors/                Collector implementation (src/collectors/pg/: libpq adapter)
src/monitor/                   faulttwin-monitor CLI
src/cli/                       faulttwin-synth (options + report are a small testable library)
tests/unit/                    GoogleTest suites
tests/integration/             Tests against the Compose lab (skipped without FT_INTEGRATION=1)
tests/cli/                     CLI determinism check (CMake script)
compose.yaml, lab/             Lab cluster definition, init and replica bootstrap scripts
.env.example                   Lab configuration template (copy to .env; never commit .env)
cmake/                         Warnings, sanitizers, pinned dependencies
scripts/verify-presets.sh      Configure/build/test every preset and save logs
scripts/run-benchmarks.sh      Release build + benchmarks + environment record
scripts/summarize-benchmarks.py  JSON -> runs.csv, summary.csv, summary.md (bootstrap CIs)
scripts/lab-init-env.sh        Create .env with random passwords
scripts/run-integration-tests.sh, measure-collection.sh  Lab tests and collection measurements
results/phase-01/ … phase-03/  Raw logs and measurement output per phase
docs/                          Architecture, ADRs, evaluation plan, evidence ledger, phase reports
.github/workflows/ci.yml       Linux CI: gcc/clang × 4 presets, clang-format, clang-tidy, lab integration
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
- [Phase 1 report](docs/phase-reports/phase-01.md), [Phase 2 report](docs/phase-reports/phase-02.md),
  [Phase 3 report](docs/phase-reports/phase-03.md)

## Development process and AI assistance

This project is designed, reviewed and owned by its sole author. An AI coding assistant is used for
implementation help, and every change is reviewed, built and committed by the author. Results are
reported only when measured.

## Licence

Not yet chosen. Third-party components keep their own licences (GoogleTest: BSD-3-Clause;
Google Benchmark: Apache-2.0; libpq and the PostgreSQL image: PostgreSQL Licence).
