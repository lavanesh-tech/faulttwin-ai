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
| 3.13 | Builds, tests and runs the lab on the owner's Mac (Apple Silicon) | ✅ owner-run on Apple Silicon, AppleClang, libpq 19.0 (Homebrew), Docker 29.6.2 / Compose v5.3.1: debug 179/179 (5 lab tests skipped), integration 7/7, monitor 3 cycles 100 % success | `results/phase-03/integration/macos-arm64.log` |
| 3.14 | CI green with the Phase 3 workflow (including the integration job) | ✅ CI run #6 on `6e23c81` green (5m 17s, seen in the owner's GitHub Actions screenshot); run #5 on `09d2e74` stopped after 1m 38s with a grey "!" status, most likely cancelled when #6 started (not confirmed) | GitHub Actions run #6 |
| 3.15 | No AI authorship; no assistant commits | ✅ | phase report §20 |
| 3.16 | Owner approval after independent review | ✅ approved by the owner ("next", 2026-11-26) | project chat |

## Phase 4 — Time-series features and anomaly detection
| # | Criterion | Status | Evidence |
|---|---|---|---|
| 4.1 | Event-time rolling windows (mean, variance, min, max, rate, slope, missing ratio, age) with documented complexity and numerically stable variance (Welford) | ✅ tested | ADR-0016, `rolling_window_test.cpp`, golden fixture |
| 4.2 | Missing, stale, invalid and not-applicable data handled explicitly (never turned into 0) | ✅ tested | `feature_extractor_test.cpp` |
| 4.3 | Versioned feature schema with pinned fingerprint; deterministic extraction | ✅ tested | `FeatureExtractorTest.*`, `cli.detect_deterministic` |
| 4.4 | Rule baseline: thresholds, sustained violations, hysteresis, cooldown, multi-signal rules, severity and escalation, stale-telemetry detection | ✅ tested | ADR-0017, `rule_engine_test.cpp` |
| 4.5 | Thresholds tuned on validation scenarios only; test splits evaluated once; overlapping splits refused | ✅ tested | `faulttwin-detect evaluate`, `cli.detect_rejects_overlapping_splits` |
| 4.6 | Detection quality measured against ground truth (precision, recall, F1, false alerts/h, latency) with a naive baseline | ✅ measured (synthetic scenarios only) | `results/phase-04/detection/` |
| 4.7 | k-NN and perceptron comparison detectors (owner request), trained and thresholded without test leakage | ✅ tested, measured (synthetic) | ADR-0018, `learned_test.cpp` |
| 4.8 | Online detection on the live lab (`faulttwin-monitor --detect`): stopped replica raises and resolves `telemetry_stale` | ✅ tested (Linux container) | `results/phase-04/integration/` |
| 4.9 | All presets pass with GCC and Clang on Linux; clang-tidy and clang-format clean | ✅ tested (Linux container) | `results/phase-04/{gcc,clang}/` |
| 4.10 | Benchmarks for windows, extraction, rules and learned scoring | ✅ measured (cloud container only) | `results/phase-04/benchmarks/` |
| 4.11 | Builds and tests on the owner's Mac | ✅ owner-run (Apple Silicon, AppleClang): debug 246/246 after a test-only fix (the golden-hash test hashed the raw `system_clock` tick count, which differs between libstdc++ and libc++); integration 8/8; `summary.json` byte-identical to Linux | `results/phase-04/integration/darwin-arm64.log` |
| 4.12 | CI green with the Phase 4 workflow | ✅ CI run #8 on `34879bf` green (6m 4s, seen in the owner's GitHub Actions screenshot), including the step that reproduces the committed detection results byte for byte | GitHub Actions run #8 |
| 4.13 | No AI authorship; no assistant commits | ✅ | phase report §20 |
| 4.14 | Owner approval after independent review | ✅ approved by the owner on 2026-11-28 ("next") | `docs/PROGRESS.md` |

## Phase 5 — Persistence, APIs and observability
| # | Criterion | Status | Evidence |
|---|---|---|---|
| 5.1 | Incident model and lifecycle: open, escalate, resolve, orphans closed at startup, write-once acknowledgement | ✅ tested | `recorder_test.cpp`, contract suite |
| 5.2 | Persistent incident store outside the monitored cluster (own network, volume, least-privilege role), versioned migrations that refuse a newer schema | ✅ tested (integration, Linux container) | ADR-0020, `tests/integration/metadb_test.cpp`, `results/phase-05/integration/` |
| 5.3 | The memory and PostgreSQL stores pass the same contract | ✅ tested (11 cases each) | `incident_store_contract.hpp` |
| 5.4 | Store outages do not lose or reorder accepted detections; the retry queue is bounded and drops are counted | ✅ tested | `RecorderTest.*` |
| 5.5 | Versioned REST API under `/api/v1` with an OpenAPI contract, strict query and body validation, one error format | ✅ tested | ADR-0021, `docs/api/openapi.yaml`, `api_service_test.cpp` |
| 5.6 | Drogon HTTP service; drains and exits 0 on SIGTERM | ✅ tested (e2e) | `scripts/e2e-faulttwind.sh`, `e2e.faulttwind_synthetic`, `integration.faulttwind_metadb` |
| 5.7 | Health and readiness reflect store reachability, monitor progress and draining | ✅ tested | `ApiTest.ReadyzNeedsAMonitorCycleAndAStore` |
| 5.8 | Prometheus metrics with bounded label cardinality; Prometheus and Grafana provisioned | ✅ tested; scrape verified (Linux container) | ADR-0019, `metrics_test.cpp`, `results/phase-05/observability/` |
| 5.9 | Structured JSON logs with secret redaction | ✅ tested | `time_logger_test.cpp` |
| 5.10 | gRPC transport only with a recorded justification | ✅ deferred with a recorded justification | ADR-0022 |
| 5.11 | All presets pass with GCC and Clang on Linux; clang-tidy and clang-format clean; Phase 4 evaluation output unchanged | ✅ tested (Linux container): 8/8 presets, sanitizers clean, Phase 4 output byte-identical | `results/phase-05/{gcc,clang}/`, phase report §14 |
| 5.12 | Builds and tests on the owner's Mac | ✅ owner-run (Apple Silicon, AppleClang, Homebrew OpenSSL 4.1 present but unused): debug 342/342 including the `faulttwind` CLI and end-to-end tests, integration 25/25 with the lab and the meta-db, after the `TRANTOR_USE_TLS=none` build fix | `results/phase-05/integration/darwin-arm64.log` |
| 5.13 | CI green with the Phase 5 workflow | ✅ CI run #10 on `3548ec2` green (8m 18s) and run #11 on `c96a6c4` green (8m 17s), seen in the owner's GitHub Actions screenshot | GitHub Actions runs #10, #11 |
| 5.14 | No AI authorship; no assistant commits; owner approval after independent review | ⏳ approval | phase report §20 |

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
