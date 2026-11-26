# Business-Impact Evidence Ledger

Purpose: every quantitative claim that may ever appear in the README, a resume or an interview must
have a row here with its evidence. **As of Phase 3 the only MEASURED rows come from one cloud
container run by the AI assistant on uncommitted code. They are not independently verified and
should not be used in a resume until the owner reproduces them on a known commit.**

Statuses: `PLANNED` · `IMPLEMENTED` · `TESTED` · `MEASURED` · `INDEPENDENTLY VERIFIED` · `NOT RUN` ·
`FAILED`. A script or test existing does not make a metric `MEASURED`. `INDEPENDENTLY VERIFIED`
requires an actual independent reproduction.

Each row records: metric ID; business problem; engineering improvement; baseline value; improved
value; absolute difference; relative improvement; workload size; independent runs; environment;
command; commit SHA; raw artifact; status; limitations; potential resume wording.

| Metric ID | Business problem | Improvement | Baseline | Improved | Abs. diff | Rel. | Workload | Runs | Env | Command | Commit | Artifact | Status | Limitations | Possible wording |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ING-TPUT / REL-THR (heavy work) | Monitoring capacity | Bounded pipeline, 2 producers × 4 consumers, capacity 16384, vs single thread; work=4096 | 233.0k events/s | 800.3k events/s | +567.3k events/s | 3.43× (+243%) | 1,000,000 synthetic events | 5 (+warm-up) | 4-vCPU cloud container, GCC 13.3, Release | `scripts/run-benchmarks.sh gcc` | `d65152f` + uncommitted Phase 2 | `results/phase-02/benchmarks/gcc/` | MEASURED (container only) | synthetic CPU-loop work; one of the best of 24 configurations at this work level, not the median configuration; Clang run in `…/clang/` | "Measured a 3.4× throughput gain over a single-threaded baseline for CPU-heavy events on 4 vCPUs" (only after the owner reproduces it) |
| ING-TPUT / REL-THR (light work) | Monitoring capacity | Same pipeline; work=0 | 4.68M events/s | 1.95M events/s (best configuration: 4 producers × 1 consumer, capacity 16384) | −2.73M events/s | 0.42× (−58%) | 1,000,000 | 5 | as above | as above | as above | as above | MEASURED (container only) | the pipeline is **slower** when per-event work is about 0.2 µs | must be reported next to the heavy-work row, never left out |
| ING-LAT-p99 | Processing delay | Bounded pipeline, 1 producer × 4 consumers, capacity 1024, work=4096 | N/A (the baseline has no queue) | 1.76 ms p99 (p50 1.09 ms) | — | — | 1,000,000 | 5 | as above | as above | as above | as above | MEASURED (container only) | with capacity 16384 and the block policy, p99 is 22–84 ms because the queue stays full (Little's law) | template only |
| ING-MEM / REL-MEM | Unbounded memory under overload | Bounded queue (capacity 1024) vs unbounded deque, one slow consumer | queue high-water 188k events; peak RSS 43.3 MiB | queue high-water 1,024 events; peak RSS 39.5 MiB | −3.8 MiB RSS | RSS −8.8%; queue depth −99.5% | 200,000 events | 5 per design, separate processes | as above | as above | as above | `overload-*.json` | MEASURED (reduced Exp. 2, container only) | RSS is dominated by the 200k-event input buffer, so it understates the queue difference; no memory cap; not the full protocol | template only |
| ING-SUCC | Lost work under overload | Explicit accounting | — | 100% of accepted events processed in every block-policy run; refused and evicted events counted and handed back in the reject/drop runs | — | — | as above | 310 GCC runs | as above | as above | as above | `runs.csv` (`invariant_ok`) | MEASURED (container only) | an accounting check, not a performance claim | template only |
| MON-SUCC | Blind spots during incidents | Collector with deadlines and reconnect | — | 100 % (900/900 node collections) | — | — | 300 cycles × 3 nodes, idle lab | 1 | 4-vCPU cloud container, lab on the same host, GCC 13.3 Release | `scripts/measure-collection.sh build/release 3` | `f0624a4` + uncommitted Phase 3 | `results/phase-03/collection/linux-x86_64-container/` | MEASURED (container only) | idle cluster only; outages are covered by integration tests, not this run | template only |
| MON-COV | Untrusted telemetry | Quality per reading | — | 100 % (16,503/16,503 applicable readings ok) | — | — | as above | 1 | as above | as above | as above | as above | MEASURED (container only) | healthy cluster; coverage under faults is tested, not measured | template only |
| MON-OVH (client) | Monitor cost | Single read-only transaction per node per cycle | — | 1.73 ms CPU per 3-node cycle (p95 2.26); cycle wall 15.2 ms mean, 20.5 ms p95 | — | — | 299 steady cycles | 1 | as above | as above | as above | `idle.jsonl.gz`, `summary.md` | MEASURED (container only) | first cycle 49 ms (connects); sequential collection | template only |
| MON-OVH (server) | Monitor cost on the database | as above | — | 0.36 ms (primary), 0.21–0.23 ms (replicas) server execution per cycle | — | — | 300 cycles | 1 | as above | as above | as above | `idle-server.csv` | MEASURED (container only) | pg_stat_statements time only (no parse, network, connection) | template only |
| MON-OVH (throughput) | Monitor slows the database | — | 5446 tps without monitor | 5391 tps at 1 s, 5249 tps at 100 ms | −55 / −197 tps | −1.0 % / −3.6 % | pgbench 4 clients × 15 s inserts | 3 interleaved per condition | as above | as above | as above | `pgbench.csv` | MEASURED (container only) | **within noise** (run spread 3.7–5.3 %); shared host | none: no claim beyond "no measurable impact at 1 s" |
| MON-FRESH | Acting on old data | Timestamp per snapshot | — | 4.3 ms mean, 10.2 ms p95, 13.5 ms p99 (report − collected_at) | — | — | 299 cycles | 1 | as above | as above | as above | `idle.jsonl.gz` | MEASURED (container only) | measures pipeline delay, not replication delay | template only |
| MON-DETLAT | Late outage detection | Deadline-bounded collection | — | stop: detected 9–13 ms after `docker compose stop` returned; pause: one 1 s deadline | — | — | single integration runs | 1 per run | Linux container | `scripts/run-integration-tests.sh` | `f0624a4` + uncommitted Phase 3 | `results/phase-03/integration/` | NOT RUN (as an experiment) | integration-test timings only, no ground-truth fault injection | template only |
| DET-F1 | Late or noisy alerts | ML predictor vs tuned rules | — | — | — | — | — | — | — | — | — | — | PLANNED | — | template only |
| REL-FA | Alert fatigue | ML predictor vs tuned rules | — | — | — | — | — | — | — | — | — | — | PLANNED | — | template only |
| DET-LEAD | Late warning | ML predictor | — | — | — | — | — | — | — | — | — | — | PLANNED | — | template only |
| REL-DT | Downtime | Twin-assisted recovery | — | — | — | — | — | — | — | — | — | — | PLANNED | — | template only |
| SIM-DT-MAPE | Trust in simulation | Calibrated twin | — | — | — | — | — | — | — | — | — | — | PLANNED | — | template only |
| REC-UNSAFE | Unsafe failover | Safety filter + ABSTAIN | — | — | — | — | — | — | — | — | — | — | PLANNED | — | template only |
| RPL-SUCC | Irreproducible incidents | Deterministic replay | — | — | — | — | — | — | — | — | — | — | PLANNED | — | template only |
| Scenario count | Resilience coverage | Chaos framework | — | — | — | — | — | — | — | — | — | — | PLANNED | — | template only |

## Phase 1 functional evidence (not resume metrics)

| Item | Status | Evidence |
|---|---|---|
| Telemetry validation rejects every injected defect kind and accepts clean samples | TESTED | `tests/unit/synthetic_test.cpp`, `cli.ground_truth_matches` |
| Synthetic generator is deterministic per seed | TESTED | `cli.deterministic_output`, `SyntheticTest.SameSeedSameSequence` |
| Test suite clean under ASan+UBSan and TSan (GCC 13.3, Clang 18.1, Linux x86_64) | TESTED | `results/phase-01/*/asan-ubsan.log`, `tsan.log` |
| CI workflow green on GitHub | TESTED | run #1 on `b36a903`, 10/10 jobs: https://github.com/lavanesh-tech/faulttwin-ai/actions/runs/45714766090 |
| Build and test on macOS (debug, AppleClang 21) | TESTED (owner-run) | `results/phase-01/macos/appleclang-debug.log` |

These are correctness facts about a small foundation. They are not performance or business
results and should not be phrased as such.

## Phase 2 functional evidence (not resume metrics)

| Item | Status | Evidence |
|---|---|---|
| Bounded queue: every policy, timeout, cancellation and close path | TESTED | `tests/unit/bounded_queue_test.cpp` |
| Exactly-once delivery or hand-back with 4 producers × 4 consumers, every policy | TESTED | `tests/unit/queue_stress_test.cpp` |
| Pipeline policies, shutdown deadline, hand-back of abandoned work | TESTED | `tests/unit/pipeline_test.cpp` |
| All tests clean under TSan and ASan+UBSan, GCC 13.3 and Clang 18.1 (Linux) | TESTED | `results/phase-02/{gcc,clang}/`, `results/phase-02/tsan-repeat/` |
| Baseline and pipeline give identical disposition counts on the same input | TESTED | `BaselineTest.MatchesThePipelineOnTheSameInput` |
| Phase 2 build and tests on macOS (debug 114/114, release 115/115, AppleClang 21) | TESTED (owner-run) | `results/phase-02/macos/appleclang-{debug,release}.ctest.log` |
| Phase 2 CI run (`3539c5c`) | TESTED (CI) | GitHub Actions `ci` run #3, green, 4m 59s |

## Phase 3 functional evidence (not resume metrics)

| Item | Status | Evidence |
|---|---|---|
| Lab cluster: primary + 2 streaming replicas, PostgreSQL 17.12 pinned by digest | TESTED (Linux container) | `LabClusterTest.Topology` |
| `ft_monitor` cannot write, create, read application data, read files or change configuration | TESTED (live server) | `MonitoringRoleIsReadOnlyAndLeastPrivileged` |
| Stopped replica reported down with every metric missing, then healthy after restart | TESTED (Linux container) | `StoppedReplicaIsDetectedAsDownAndRecovers` |
| Frozen replica costs one client deadline, no hang | TESTED (Linux container) | `PausedReplicaTimesOutInsteadOfHanging` |
| Missing / stale / invalid / not-applicable kept apart from healthy values | TESTED | `tests/unit/collectors/` |
| No password in argv, logs, JSON output or `postgresql.auto.conf` | CHECKED | replica entrypoint guard, `cli.monitor_requires_password` |
| Phase 3 on macOS and in CI | NOT RUN | owner steps |
