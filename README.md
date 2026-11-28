# FaultTwin AI

**AI-Powered Distributed Database Failure Prediction and Recovery Engine** — an experimental
reliability-engineering platform, written primarily in C++20, that will collect PostgreSQL cluster
telemetry, detect anomalies, predict developing failures, simulate recovery strategies on a custom
discrete-event digital twin and recommend evidence-based actions. It is advisory: it does not
execute destructive or production recovery actions.

> **Status: Phase 5 of 12 (incident storage, HTTP API, self-observability) submitted for review.**
> What exists today is the build system, the telemetry value model with validation, a
> synthetic-data CLI, a bounded concurrent ingest pipeline, a Docker Compose PostgreSQL lab
> (primary + 2 streaming replicas), a read-only PostgreSQL and host collector
> (`faulttwin-monitor`), event-time rolling-window features, a rule-based anomaly detector with an
> evaluation harness on synthetic scenarios (`faulttwin-detect`), the `faulttwind` daemon (monitor
> loop, incidents stored in a separate PostgreSQL meta-db, a versioned HTTP API on Drogon,
> Prometheus metrics, JSON logs), and tests, including integration tests against the lab. gRPC is
> designed for but not built (ADR-0022). Measurements so far come from one cloud container and are not
> independently reproduced; detection quality has only been measured on synthetic data. Everything
> else is design. See [`docs/PROGRESS.md`](docs/PROGRESS.md).

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
- `faulttwin-monitor` (needs libpq): collects from the lab and prints text or JSON lines;
  `--detect` also runs the feature extractor and the rules on every cycle.
- `faulttwin_features`: event-time rolling windows (fixed-capacity ring buffers, Welford mean and
  variance with periodic exact re-seed), last/mean/stddev/min/max/rate/slope/missing-ratio/age per
  metric, a versioned feature schema (`faulttwin.features.v1`, 90 columns, pinned fingerprint) and
  adapters from collector snapshots and Phase 1 samples. Missing data is never turned into 0.
- `faulttwin_detection`: rules with sustained violations, hysteresis, cooldown, multi-signal
  (`all`/`any`, three-valued with missing data), severity and escalation, and stale-telemetry
  detection; deterministic synthetic fault scenarios with ground truth; alert/episode scoring
  (precision, recall, F1, false alerts per hour, detection latency) and per-rule tuning.
- `faulttwin-detect`: tunes the rules on validation scenarios only, then evaluates them and a
  naive-threshold baseline once on held-out test scenarios. Two learned comparison detectors, a
  k-NN distance detector (unsupervised) and an averaged perceptron (supervised), train on the
  first half of the validation seeds, pick their alert thresholds on the second half and are
  evaluated on the same test splits (ADR-0018).
- `faulttwin_storage`: incidents (one alert lifetime per rule and node, at most 3 events,
  write-once acknowledgement), an `IncidentStore` interface with a bounded in-memory store and a
  PostgreSQL store on a separate meta-db (single-statement writes, bind parameters only,
  versioned migrations), and a recorder with a bounded, ordered retry queue for store outages
  (ADR-0020).
- `faulttwin_api`, `faulttwin_observability`, `faulttwin_core`: the HTTP API as a
  transport-free service (routing, strict query and body validation, Bearer auth for writes,
  one error format, request ids), a Prometheus metrics registry with bounded label sets, JSON-lines
  logging with secret redaction, and a strict bounded JSON parser (ADR-0019, ADR-0021).
- `faulttwind` (needs libpq and jsoncpp): runs the monitor loop against the lab (or a synthetic
  scenario), records incidents, and serves `/api/v1`, `/healthz`, `/readyz` and `/metrics` on
  Drogon. Loopback only unless `--allow-remote`; drains cleanly on SIGTERM.

## Build and test

Requirements: CMake ≥ 3.25, Ninja, GCC ≥ 13 or Clang ≥ 18, network access to github.com for the
first configure (GoogleTest is fetched at a pinned commit). Sanitizer builds with Clang on Ubuntu
also need `libclang-rt-18-dev`. The PostgreSQL collector and its tests need libpq
(`sudo apt-get install libpq-dev` or `brew install libpq`; found automatically on macOS); without
it they are skipped and everything else still builds. `faulttwind` also needs jsoncpp
(`sudo apt-get install libjsoncpp-dev uuid-dev zlib1g-dev` or `brew install jsoncpp`); Drogon itself
is fetched at a pinned commit on the first configure. Without jsoncpp `faulttwind` is skipped.

```bash
cmake --preset debug          # or: release, asan-ubsan, tsan
cmake --build --preset debug
ctest --preset debug

# Everything, with full logs saved under results/phase-05/<compiler>/
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

## faulttwind (incidents and HTTP API)

```bash
set -a; . ./.env; set +a                 # FT_API_KEY, FT_METADB_*, FT_MONITOR_*
./build/debug/src/server/faulttwind --source postgres --store postgres   # live lab, meta-db
./build/debug/src/server/faulttwind                                      # synthetic, in memory
curl -s 127.0.0.1:8080/readyz
curl -s '127.0.0.1:8080/api/v1/incidents?state=open&limit=10'
curl -s -X POST -H "Authorization: Bearer $FT_API_KEY" -H 'Content-Type: application/json' \
     -d '{"by":"me","note":"looking"}' 127.0.0.1:8080/api/v1/incidents/1/acknowledge
docker compose --profile observability up -d   # Prometheus on :59090, Grafana on :53000
```

The API contract is [`docs/api/openapi.yaml`](docs/api/openapi.yaml). The synthetic source replays
a Phase 4 scenario, so every value it reports is generated, not measured.

## Anomaly detection

```bash
./build/release/src/detect/faulttwin-detect evaluate --out /tmp/detect   # tune on seeds 1-40, test on 1001-1040 and 2001-2040
./build/release/src/detect/faulttwin-detect scenario --seed 3            # one scenario: ground truth and detections
./build/debug/src/monitor/faulttwin-monitor --cycles 30 --detect         # live: rules on the lab (needs .env loaded)
```

The scenarios are synthetic and illustrative; their results show how the rules behave, not how
well they detect real PostgreSQL failures (that needs the Phase 6 lab datasets).

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
include/faulttwin/features/    Observation, rolling window, feature schema, extractor, adapters
include/faulttwin/detection/   Rules, rule engine, online detector, scenarios, evaluation, JSON
include/faulttwin/core/        JSON parser and writer, time conversion, structured logger
include/faulttwin/observability/  Prometheus metrics registry
include/faulttwin/storage/     Incident model, store interface, memory and PostgreSQL stores, recorder
include/faulttwin/api/         HTTP request/response, API service, status board
src/telemetry/                 Library implementation
src/concurrency/, src/ingest/  Library implementations
bench/                         Google Benchmark suite (Release only)
src/collectors/                Collector implementation (src/collectors/pg/: libpq adapter)
src/monitor/                   faulttwin-monitor CLI
src/features/, src/detection/  Feature and detection libraries
src/detect/                    faulttwin-detect CLI (tuning and evaluation on synthetic scenarios)
src/cli/                       faulttwin-synth (options + report are a small testable library)
src/core/, src/observability/, src/storage/, src/api/  Phase 5 libraries
src/server/                    faulttwind (options, monitor loop, Drogon adapter, main)
tests/unit/                    GoogleTest suites
tests/integration/             Tests against the Compose lab (skipped without FT_INTEGRATION=1)
tests/cli/                     CLI determinism checks (CMake scripts)
compose.yaml, lab/             Lab cluster and meta-db definition, init and replica bootstrap scripts
infra/                         Prometheus scrape config, Grafana provisioning and dashboard
docs/api/openapi.yaml          HTTP API contract (v1)
.env.example                   Lab configuration template (copy to .env; never commit .env)
cmake/                         Warnings, sanitizers, pinned dependencies
scripts/verify-presets.sh      Configure/build/test every preset and save logs
scripts/run-benchmarks.sh      Release build + benchmarks + environment record
scripts/summarize-benchmarks.py  JSON -> runs.csv, summary.csv, summary.md (bootstrap CIs)
scripts/lab-init-env.sh        Create .env with random passwords
scripts/run-integration-tests.sh, measure-collection.sh  Lab tests and collection measurements
scripts/features-reference.py  Exact-arithmetic reference for the feature golden fixture
scripts/run-detection-benchmarks.sh  Release build + Phase 4 benchmarks
scripts/e2e-faulttwind.sh      Start faulttwind and check it over real HTTP
results/phase-01/ … phase-05/  Raw logs and measurement output per phase
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
  [Phase 3 report](docs/phase-reports/phase-03.md), [Phase 4 report](docs/phase-reports/phase-04.md),
  [Phase 5 report](docs/phase-reports/phase-05.md)
- [HTTP API (OpenAPI)](docs/api/openapi.yaml)

## Development process and AI assistance

This project is designed, reviewed and owned by its sole author. An AI coding assistant is used for
implementation help, and every change is reviewed, built and committed by the author. Results are
reported only when measured.

## Licence

Not yet chosen. Third-party components keep their own licences (GoogleTest: BSD-3-Clause;
Google Benchmark: Apache-2.0; libpq and the PostgreSQL image: PostgreSQL Licence; Drogon: MIT;
Trantor: BSD-3-Clause; jsoncpp: MIT; Prometheus and Grafana images: Apache-2.0 and AGPL-3.0, used
as separate services only).
