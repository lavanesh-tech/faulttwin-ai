# FaultTwin AI — Acceptance Criteria by Phase

Phase numbering follows the owner's master specification (Phases 0–12). A phase is complete only
when every criterion is met, verified by a command whose real output is saved, **and the owner has
approved it after technical review**. Targets marked *(T)* are planning targets from
`docs/evaluation/metric-targets.md`: the criterion is "measured and reported honestly", never "the
target was hit".

Status legend: ✅ met (evidence linked) · ⏳ not yet verified · ❌ not met · — not applicable yet.

## Phase 0 — Architecture and evaluation design
| # | Criterion | Status |
|---|---|---|
| 0.1 | Scope, business problems, MVP boundaries documented (`docs/architecture/requirements.md`) | ✅ |
| 0.2 | Architecture, module boundaries and safety model documented (`docs/architecture/overview.md`) | ✅ |
| 0.3 | Metric definitions, baselines, experiment protocols, targets (`docs/evaluation/`) | ✅ drafted |
| 0.4 | ADRs for decisions with real alternatives (`docs/adr/`) | ✅ |
| 0.5 | Owner approval after review | ✅ approved 2026-11-25 |

## Phase 1 — C++20 engineering foundation
| # | Criterion | Status | Evidence |
|---|---|---|---|
| 1.1 | Root `CMakeLists.txt` and `CMakePresets.json`; in-source builds rejected | ✅ | files |
| 1.2 | C++20 library (`faulttwin_telemetry`) and executable (`faulttwin-synth`) targets | ✅ | `src/` |
| 1.3 | GoogleTest pinned to a commit SHA | ✅ | `cmake/Dependencies.cmake` |
| 1.4 | Debug and Release presets build and pass tests with GCC and Clang on Linux | ✅ | `results/phase-01/{gcc,clang}/{debug,release}.log` |
| 1.5 | Separate ASan+UBSan and TSan presets build and pass tests with GCC and Clang on Linux | ✅ | `results/phase-01/*/{asan-ubsan,tsan}.log` |
| 1.6 | Strict warnings with `-Werror` for first-party code only; build is warning-free | ✅ | build logs |
| 1.7 | Telemetry model has every field required by the specification | ✅ | `telemetry_sample.hpp` |
| 1.8 | Every required validation case has a deterministic unit test | ✅ | `tests/unit/validation_test.cpp`, `node_id_test.cpp`, `node_role_test.cpp` |
| 1.9 | Missing replication lag is distinguishable from zero lag, with tests | ✅ | `MissingLagOnReplicaIsNotZeroLag`, `ZeroLagOnReplicaIsAMeasurement` |
| 1.10 | Synthetic CLI runs, is deterministic for a seed, and self-checks against injected ground truth | ✅ | `cli.deterministic_output`, `cli.ground_truth_matches` |
| 1.11 | clang-format clean; clang-tidy clean on first-party sources with the committed config | ✅ | phase report §12–14 |
| 1.12 | Linux GitHub Actions workflow exists | ✅ | `.github/workflows/ci.yml` |
| 1.13 | **The workflow has actually run green on GitHub** | ✅ run #1 on `b36a903`, 10/10 jobs, 4m 45s | [run 45714766090](https://github.com/lavanesh-tech/faulttwin-ai/actions/runs/45714766090) |
| 1.14 | Builds and tests on the owner's macOS machine | ✅ debug, 57/57, AppleClang 21 | `results/phase-01/macos/appleclang-debug.log` |
| 1.15 | README, ADRs, metric definitions, evaluation plan, evidence ledger with no results | ✅ | `docs/` |
| 1.16 | No AI listed as author/co-author anywhere; no commits made by the assistant | ✅ | phase report §20 |
| 1.17 | Owner approval after independent review | ✅ approved 2026-11-25 ("next") | |

## Phase 2 — Concurrent telemetry ingestion
| # | Criterion | Status | Evidence |
|---|---|---|---|
| 2.1 | Bounded thread-safe queue with explicit overload policies (block with timeout, reject newest, drop oldest) | ✅ implemented, tested | `bounded_queue.hpp`, `bounded_queue_test.cpp` |
| 2.2 | Counters for attempted/accepted/processed/rejected/dropped; nothing discarded silently (refused/evicted/abandoned items handed back) | ✅ tested | `IngestCounters`, `QueueStats`, exactly-once stress tests, `invariant_ok` in every benchmark run |
| 2.3 | Producers/consumers with `std::jthread` and `std::stop_token` | ✅ tested | `pipeline.hpp`, `pipeline_test.cpp` |
| 2.4 | Graceful shutdown with a drain deadline | ✅ tested | `ShutdownDeadlineAbandonsAndReturnsUnprocessedEvents`, `CancelledConsumerHandsBackTheRestOfItsBatch` |
| 2.5 | Cancellation and timeout tests | ✅ tested | `*TimesOut*`, `*Cancelled*` tests |
| 2.6 | Duplicate, invalid and out-of-order policies with tests | ✅ tested | `order_tracker_test.cpp`, `Policies/PipelinePolicyTest.*` |
| 2.7 | Concurrency stress tests pass under TSan (GCC and Clang, Linux) | ✅ tested | `results/phase-02/{gcc,clang}/tsan.log`, `results/phase-02/tsan-repeat/` |
| 2.8 | All presets pass with GCC and Clang on Linux | ✅ tested | `results/phase-02/{gcc,clang}/*.log` |
| 2.9 | Single-threaded baseline implemented with identical work | ✅ tested | `baseline.hpp`, `BaselineTest.MatchesThePipelineOnTheSameInput` |
| 2.10 | Release benchmarks recorded per Experiment 1 with machine-readable output and environment metadata *(T)* | ✅ measured (cloud container only; deviations recorded in `experiment-protocols.md`) | `results/phase-02/benchmarks/` |
| 2.11 | Experiment 2 (overload) recorded *(T)* | ⏳ reduced version only | `overload-*.json`; full protocol NOT RUN |
| 2.12 | Builds and tests on the owner's macOS machine | ✅ debug 114/114, release 115/115, AppleClang 21 | `results/phase-02/macos/appleclang-{debug,release}.ctest.log` |
| 2.13 | CI green on GitHub with the Phase 2 code | ✅ CI run #3 on `3539c5c` green (4m 59s, seen in the owner's GitHub Actions screenshot) | GitHub Actions run #3 |
| 2.14 | No AI authorship; no assistant commits | ✅ | phase report §20 |
| 2.15 | Owner approval after independent review | ⏳ | |

## Phase 3 — PostgreSQL monitoring
| # | Criterion | Status | Evidence |
|---|---|---|---|
| 3.1 | Docker Compose lab: primary + 2 streaming replicas, PostgreSQL version pinned, isolated network, ports on localhost only | ✅ implemented, tested (Linux container) | `compose.yaml`, ADR-0014, `LabClusterTest.Topology` |
| 3.2 | Least-privilege read-only monitoring role, denials proven on a live server | ✅ tested | `lab/primary/init/10-faulttwin-lab.sh`, `MonitoringRoleIsReadOnlyAndLeastPrivileged` |
| 3.3 | Secrets only in `.env` (git-ignored), `.env.example` committed, no password in argv, logs or `postgresql.auto.conf` | ✅ implemented, checked | `.env.example`, `scripts/lab-init-env.sh`, replica entrypoint check, `cli.monitor_requires_password` |
| 3.4 | Collectors: availability, connections, transaction throughput, query latency, WAL, replication state and lag, replay positions, role, topology | ✅ tested | `pg_collector_test.cpp`, `rates_topology_test.cpp`, `LabClusterTest.*` |
| 3.5 | Linux resource monitoring (CPU, memory, load, disk) | ✅ tested | `linux_resources_test.cpp` |
| 3.6 | Missing, stale, invalid and not-applicable telemetry distinguished from healthy values | ✅ tested | `Reading<T>`, `StalenessIsSeparateFromMissing`, `GarbageValuesAreInvalidNotZero`, `CounterResetIsInvalidNotNegative` |
| 3.7 | Error handling: refused, auth, permission, timeout, unexpected result, missing extension | ✅ tested | `pg_collector_test.cpp`, `LibpqSessionTest.*` |
| 3.8 | Integration test: stopped replica detected as down and recovers | ✅ tested (Linux container) | `StoppedReplicaIsDetectedAsDownAndRecovers`, `results/phase-03/integration/` |
| 3.9 | Integration test: frozen replica costs one deadline, no hang | ✅ tested (Linux container) | `PausedReplicaTimesOutInsteadOfHanging` |
| 3.10 | Integration tests skip cleanly without Docker (`FT_INTEGRATION` unset) | ✅ tested | normal preset runs report 5 tests Skipped |
| 3.11 | All presets pass with GCC and Clang on Linux; clang-tidy and clang-format clean | ✅ tested (Linux container): 4 presets × 2 compilers; tidy and format clean | `results/phase-03/{gcc,clang}/*.log` |
| 3.12 | Collection success, coverage, cost, freshness and throughput impact measured *(T)* | ✅ measured (cloud container only) | `results/phase-03/collection/linux-x86_64-container/` |
| 3.13 | Builds, tests and runs the lab on the owner's Mac (Apple Silicon) | ⏳ NOT RUN | owner steps |
| 3.14 | CI green with the Phase 3 workflow (including the integration job) | ⏳ NOT RUN | `ci.phase-03.yml` → `.github/workflows/ci.yml` |
| 3.15 | No AI authorship; no assistant commits | ✅ | phase report §20 |
| 3.16 | Owner approval after independent review | ⏳ | |

## Phase 4 — Time-series features and anomaly detection
- Rolling-window features with documented complexity and numerically stable variance (Welford).
- Versioned feature schema; deterministic extraction tests.
- Rule baseline with sustained violations, hysteresis, cooldown, multi-signal rules, severity and
  stale-telemetry detection; thresholds tuned on validation scenarios only.

## Phase 5 — Persistence, APIs and observability
- Incident storage, versioned Drogon REST API, health/readiness, Prometheus metrics, structured logs.
- gRPC transport only with a recorded justification (ADR).

## Phase 6 — Chaos engineering and dataset generation
- Scenario files with all ten fields from Module G; ≥ 30 distinct configurations *(T)*.
- Environment guards tested negatively (each guard failing causes refusal).
- Datasets with ground-truth labels and manifests; reproducible orchestration.

## Phase 7 — ML training and native C++ inference
- Leakage-safe splits per `docs/evaluation/experiment-protocols.md`; LR/RF/GB compared.
- ONNX export; Python↔C++ parity within a stated tolerance; fallback and abstention paths tested.
- Experiment 3 and 6 results recorded, including negative results *(T)*.

## Phase 8 — Custom digital twin
- Priority-queue scheduler with total `(time, sequence)` ordering; logical clock; seeded RNG.
- Determinism test: identical trace hash across repeated runs.
- Calibration scenarios separate from evaluation scenarios; Experiment 5 recorded *(T)*.

## Phase 9 — Recovery evaluation and ranking
- Safety filter before ranking; ABSTAIN outcome; approval records; Experiments 4 and 8 recorded.

## Phase 10 — AI explanations and dashboard
- Structured evidence, citation validation, prompt-injection tests, cost limits, no-key fallback.
- Dashboard distinguishes measured, predicted and illustrative values.

## Phase 11 — Performance optimization and hardening
- Profiling-driven optimizations with before/after Release benchmarks; threat model; security and
  sanitizer campaigns; final experiment reports.

## Phase 12 — Portfolio, documentation and interview preparation
- Every README and resume claim traces to a `MEASURED` (or independently verified) ledger entry.
