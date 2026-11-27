# FaultTwin AI — Requirements and MVP Boundaries

Status: **Phase 0 design document.** The owner's master specification is the source of truth; this
file condenses it into stable requirement IDs, MVP boundaries and the assumptions behind them.
Implemented so far: FR-TEL-1/2 (Phase 1), FR-TEL-3/4/5 (Phase 2), FR-MON-1/2/3 (Phase 3) and FR-FEAT-1, FR-DET-1 (Phase 4, detection measured on synthetic scenarios only).

## 1. Problem statement

Operators of replicated PostgreSQL clusters often learn about failures (replication lag spirals,
connection exhaustion, an unreachable primary) after users do, and then choose a recovery action
under pressure with little evidence about its consequences. Threshold monitoring reports symptoms;
HA managers execute fixed failover policies.

FaultTwin AI investigates whether combining predictive monitoring, deterministic simulation and
evidence-based recovery evaluation **improves reliability decisions in controlled lab experiments**.
It is advisory: it never executes destructive or production recovery actions on its own. Laboratory
results are not presented as commercial or production outcomes.

## 2. Functional requirements (by module of the master specification)

| ID | Requirement | Module | Phase |
|---|---|---|---|
| FR-TEL-1 | Telemetry value model: bounded node id, `NodeRole` scoped enum, `std::chrono` wall-clock timestamp, CPU/memory/disk percentages, optional replication lag (ms) | A | 1 ✅ |
| FR-TEL-2 | Deterministic validation: empty/over-long ids, invalid roles, out-of-range percentages, NaN/∞, negative lag, explicit timestamp policy, role-specific lag semantics; missing lag ≠ zero lag | A | 1 ✅ |
| FR-TEL-3 | Bounded MPMC queue, producers/consumers, backpressure with explicit overload policies, cancellation, timeouts, graceful shutdown | A | 2 |
| FR-TEL-4 | Duplicate, invalid and out-of-order sample policies; attempted/accepted/processed/rejected/dropped counters (no silent discarding) | A | 2 |
| FR-TEL-5 | Throughput, latency percentile and memory instrumentation; single-threaded baseline | A | 2 |
| FR-MON-1 | PostgreSQL collectors with read-only credentials: availability, connections, transaction throughput, query latency, WAL generation, replication lag/state, replay positions, role/topology | B | 3 |
| FR-MON-2 | Host/container resources: CPU, memory, disk capacity, disk I/O pressure, network latency/failures | B | 3 |
| FR-MON-3 | Distinguish missing, stale and invalid telemetry from healthy values | B | 1 (model) / 3 |
| FR-FEAT-1 | Rolling windows, moving average, variance, rate of change, trend, missing-value handling, event-time semantics, versioned feature schema, deterministic extraction | C | 4 (implemented, tested) |
| FR-DET-1 | Rule baseline: thresholds, sustained violations, hysteresis, cooldown, multi-signal conditions, severity, stale-telemetry detection; tuned on validation data | D | 4 (implemented, tested; tuned on synthetic validation scenarios until Phase 6; k-NN and perceptron comparison detectors added, ADR-0018) |
| FR-ML-1 | Train LR / RF / gradient boosting in scikit-learn on leakage-safe splits; select empirically; export ONNX; model metadata | E | 7 |
| FR-ML-2 | Native ONNX Runtime inference in C++; Python↔C++ parity tests; deterministic fallback; explicit abstention | E | 7 |
| FR-TWIN-1 | Custom discrete-event simulator: logical clock, priority-queue scheduling, deterministic same-timestamp ordering, seeded randomness | F | 8 |
| FR-TWIN-2 | Model primary/replicas, health states, connectivity, replication, WAL propagation, backlog, workload, recovery delays, availability | F | 8 |
| FR-CHAOS-1 | Reproducible lab-only experiments: latency, interruption, connection failures, crashes, stalled replication, CPU/memory/disk-I/O pressure, workload spikes, telemetry loss, overlapping faults | G | 6 |
| FR-CHAOS-2 | Each scenario: id, topology, workload, fault config, seed, ground-truth timestamps, failure definition, invariants, recovery criteria, outcome | G | 6 |
| FR-REC-1 | Strategies: continue monitoring, reduce workload, wait for replica catch-up, restore connectivity, restart failed database, prepare controlled failover, promote eligible replica after safety checks | H | 9 |
| FR-REC-2 | Reject unsafe strategies before ranking; explicit ABSTAIN; never prefer an unsafe promotion for lower downtime | H | 9 |
| FR-REC-3 | Advisory mode; any later lab execution requires human approval, authorization, topology revalidation, fencing checks, audit | H | 9 |
| FR-INC-1 | Record telemetry, detections, topology, model + feature-schema versions, rule and simulation config, seeds, strategies, evidence, approvals, observed outcomes | I | 5 / 9 |
| FR-INC-2 | Deterministic replay with stable event ordering | I | 8 / 9 |
| FR-LLM-1 | OpenAI explanations from structured evidence only; cite evidence; separate facts from hypotheses; no tools, no infrastructure access; prompt-injection resistant | J | 10 |
| FR-UI-1 | React/TypeScript dashboard; measured vs predicted vs illustrative values always distinguished | K | 10 |
| FR-API-1 | Versioned Drogon REST API, health/readiness, Prometheus metrics, structured logs | — | 5 |

## 3. Non-functional requirements

| ID | Requirement |
|---|---|
| NFR-1 | C++20; strict warnings as errors for first-party code; clang-format and clang-tidy enforced in CI. |
| NFR-2 | RAII, Rule of Zero, value semantics, const correctness; smart pointers only for real ownership; no raw owning pointers; minimal global state. |
| NFR-3 | Thread-safe interfaces with explicit cancellation and shutdown; no detached threads; no busy waiting. |
| NFR-4 | Bounded memory: every queue, cache and store has a configured capacity. |
| NFR-5 | Explicit error contracts: invalid input is a value (result/issue list), not an exception; exceptions only for exceptional conditions; none escape `main` or thread entry points. |
| NFR-6 | Determinism where required (validation, features, simulation, replay). |
| NFR-7 | Reproducible builds: pinned dependency versions (commit SHAs), CMake presets, documented toolchain. |
| NFR-8 | Sanitizer-clean (ASan+UBSan, TSan) test suites; sanitizer builds never used for performance numbers. |
| NFR-9 | Every reported metric is traceable to a command, commit, environment and raw artifact (see `docs/evaluation/`). |
| NFR-10 | Security proportionate to each phase: least privilege, input validation, no hard-coded secrets, lab isolation, audit. |

## 4. MVP boundaries

**In scope:** one Docker Compose host (Linux, or Docker Desktop on macOS); one PostgreSQL primary and
two asynchronous streaming replicas; Toxiproxy on client and replication paths; the module set above.

**Out of scope:** any non-lab database; Kubernetes, Kafka, Redis or extra AI frameworks (excluded by
the master specification unless an ADR justifies them); HA managers and automatic failover;
synchronous/logical replication; engines other than PostgreSQL; multi-tenant RBAC/SSO; online
retraining.

**Deferred with a trigger:** gRPC telemetry transport (when a remote collector exists, ADR-0003);
separate simulation worker process (only if measured contention justifies it, ADR-0002).

## 5. Assumptions

- A1. The owner develops on macOS (Apple silicon) and builds/tests on Linux (container or CI).
- A2. OpenAI API access is optional; the system must work without it.
- A3. Container resource metrics reflect cgroup limits inside the Docker VM, not physical hardware.
- A4. Lab clusters are small (three nodes); results are only claimed at that scale.
- A5. Wall-clock timestamps come from collector hosts and may drift; validation tolerates a bounded
  future skew (default 5 s) and treats age as staleness, not invalidity.

## 6. Open questions for the owner

1. Project licence (not chosen yet).
2. Wording and location of the AI-assistance disclosure (a short README note is in place as a draft).
