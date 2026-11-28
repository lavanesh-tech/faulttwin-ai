# Verified Results

A row is added here only when a metric reaches status `MEASURED` with a raw artifact, commit SHA,
environment record and command, and it is marked `INDEPENDENTLY VERIFIED` only after someone other
than the implementer has reproduced it.

**Nothing here is independently verified.** The Phase 2 rows were measured by the AI
implementation assistant in one 4-vCPU Linux cloud container (Intel Xeon @ 2.10 GHz, shared host),
on the owner's commit `d65152f` plus uncommitted Phase 2 changes. Values are medians of 5
repetitions; the 95% bootstrap CIs and every raw run are in the artifacts.

| Metric ID | Configuration | Value (GCC 13.3) | Value (Clang 18.1) | Runs | Commit | Artifact | Status |
|---|---|---|---|---|---|---|---|
| ING-TPUT (baseline) | single thread, work=0 | 4.68M events/s | 5.13M events/s | 5 | `d65152f` + uncommitted | `results/phase-02/benchmarks/{gcc,clang}/` | MEASURED (container) |
| ING-TPUT (baseline) | single thread, work=4096 | 233.0k events/s | 176.2k events/s | 5 | `d65152f` + uncommitted | `results/phase-02/benchmarks/{gcc,clang}/` | MEASURED (container) |
| ING-TPUT / REL-THR | pipeline 2×4, capacity 16384, work=4096 | 800.3k events/s (3.43×) | 588.7k events/s (3.34×) | 5 | `d65152f` + uncommitted | `results/phase-02/benchmarks/{gcc,clang}/` | MEASURED (container) |
| ING-TPUT / REL-THR | best pipeline row, work=0 | 1.95M events/s (0.42×) | 2.14M events/s (0.42×) | 5 | `d65152f` + uncommitted | `results/phase-02/benchmarks/{gcc,clang}/` | MEASURED (container) |
| REL-THR range | all 24 pipeline rows, work=0 / work=4096 | 0.09–0.42× / 0.94–3.50× | 0.10–0.42× / 0.94–3.46× | 5 each | `d65152f` + uncommitted | `results/phase-02/benchmarks/{gcc,clang}/` | MEASURED (container) |
| ING-SCALE | 2 / 4 / 8 consumers, work=4096 | 0.83–0.92 / 0.73–0.84 / 0.37–0.43 | 0.91–0.97 / 0.80–0.88 / 0.42–0.45 | 5 each | `d65152f` + uncommitted | `results/phase-02/benchmarks/{gcc,clang}/` | MEASURED (container) |
| ING-LAT-p99 | pipeline 1×4, capacity 1024, work=4096 | 1.76 ms | 2.20 ms | 5 | `d65152f` + uncommitted | `results/phase-02/benchmarks/{gcc,clang}/` | MEASURED (container) |
| ING-LAT-p99 | worst row, capacity 16384, work=4096 | 83.7 ms | 103.1 ms | 5 | `d65152f` + uncommitted | `results/phase-02/benchmarks/{gcc,clang}/` | MEASURED (container) |
| Exp. 2 reduced: queue high-water | bounded (1024) vs unbounded, 200k events | 1,024 vs 188k | 1,024 vs 192k | 5 | `d65152f` + uncommitted | `overload-*.json` | MEASURED (container, reduced protocol) |
| Exp. 2 reduced: ING-MEM peak RSS | bounded block vs unbounded | 39.5 vs 43.3 MiB | 39.4 vs 43.2 MiB | 5 | `d65152f` + uncommitted | `overload-*.json` | MEASURED (container, reduced protocol) |
| ING-SUCC | block-policy runs | 100% | 100% | 310 per compiler | `d65152f` + uncommitted | `runs.csv` | MEASURED (container) |

## Phase 3 (PostgreSQL monitoring)

Measured by the AI assistant in one 4-vCPU Linux cloud container, with the 3-node lab, the
monitor and pgbench on the same host; GCC 13.3 Release; `f0624a4` + uncommitted Phase 3.
**Not independently verified; not run on macOS or in CI.**

| Metric ID | Configuration | Value | Runs | Artifact | Status |
|---|---|---|---|---|---|
| MON-SUCC | idle lab, 300 cycles × 3 nodes, 200 ms | 100 % (900/900) | 1 | `results/phase-03/collection/linux-x86_64-container/` | MEASURED (container) |
| MON-COV | as above | 100 % (16,503/16,503) | 1 | as above | MEASURED (container) |
| MON-OVH client | cycles 2–300 | CPU 1.73 ms mean / 2.26 ms p95 per cycle; wall 15.16 ms mean / 20.51 p95 / 28.65 p99 | 1 | `summary.md` | MEASURED (container) |
| MON-OVH server | pg_stat_statements, ft_monitor | 0.359 / 0.233 / 0.209 ms per cycle (primary / replica-1 / replica-2) | 1 | `idle-server.csv` | MEASURED (container) |
| MON-OVH throughput | pgbench 4 × 15 s, none / 1 s / 100 ms | 5446 / 5391 / 5249 tps mean (−1.0 % / −3.6 %), within noise | 3 each | `pgbench.csv` | MEASURED (container) |
| MON-FRESH | cycles 2–300 | 4.27 ms mean / 10.23 p95 / 13.47 p99 | 1 | `idle.jsonl.gz` | MEASURED (container) |

Phase 1 produced **functional test results** (pass/fail), not metrics. They are recorded in
`docs/phase-reports/phase-01.md` and `results/phase-01/`, not here.

## Phase 4 (features and anomaly detection)

Measured by the AI assistant in one 4-vCPU Linux cloud container, base `8f1dbb2` plus
uncommitted Phase 4 changes. **Not independently verified; not run on macOS or in CI.**
Detection quality is on deterministic **synthetic** scenarios only.

| Item | Configuration | Value | Artifact | Status |
|---|---|---|---|---|
| Tuned rules, test split | seeds 1001–1040, 120 episodes | P 100 %, R 99.2 %, F1 0.996, 0 false alerts | `results/phase-04/detection/linux-x86_64-container/summary.json` | MEASURED (synthetic) |
| Naive threshold, test split | same | P 88.5 %, R 91.7 %, F1 0.900, 1.86 false alerts/h | as above | MEASURED (synthetic) |
| k-NN distance, test split | train 1–20, thresholds 21–40 | P 94.7 %, R 90.0 %, F1 0.923, 0.22 false alerts/h | as above | MEASURED (synthetic) |
| Averaged perceptron, test split | same | P 86.0 %, R 40.8 %, F1 0.554 (CPU saturation and overload 0 detected) | as above | MEASURED (synthetic) |
| Shifted split (weaker faults) | seeds 2001–2040 | F1: rules 0.970, naive 0.749, k-NN 0.904, perceptron 0.638 | as above | MEASURED (synthetic) |
| Live stale-telemetry detection | `docker compose stop` of a replica, 1 s cycles | fired 3037–3046 ms after stop returned; resolved 6097–6125 ms after start | `results/phase-04/integration/` | MEASURED (container, 3 single runs) |
| Benchmarks (GCC / Clang Release, median of 5) | see phase report §16 | rule evaluation 143 / 141 ns; feature vector 5.0 / 3.5 µs; k-NN score 11.9 / 11.2 µs; perceptron 6.9 / 6.6 ns | `results/phase-04/benchmarks/{gcc,clang}/` | MEASURED (container) |

## Phase 5 (persistence, APIs and observability)

Run by the AI assistant in one 4-vCPU Linux cloud container, base `6209dee` plus uncommitted
Phase 5 changes. **Not independently verified; not run in CI. Owner-run on macOS: debug 342/342, integration 25/25.** These are
functional results and one build-cost measurement. No API performance is claimed.

| Item | Configuration | Value | Artifact | Status |
|---|---|---|---|---|
| Integration suite with the meta-db | GCC debug, `FT_INTEGRATION=1` | 25/25 passed (8 lab, 16 meta-db, 1 daemon e2e on PostgreSQL) | `results/phase-05/integration/linux-x86_64-container.log` | TESTED (container) |
| Presets | GCC + Clang × debug, release, asan-ubsan, tsan | all 8 pass (342 tests; 344 in release); ASan, UBSan and TSan clean | `results/phase-05/{gcc,clang}/` | TESTED (container) |
| Phase 4 evaluation output after the refactor | GCC debug | 8/8 files byte-identical to `results/phase-04/detection/` | phase report §14 | TESTED (container) |
| Prometheus scrape and Grafana provisioning | Prometheus 3.5.0, Grafana 12.1.0, faulttwind on docker0 | `up` = 1; datasource health OK; dashboard provisioned | `results/phase-05/observability/linux-x86_64-container.txt` | TESTED (container) |
| Drogon build cost | release, fresh build dir, -j4, GCC 13.3 | 50.4 s (rest of the project 66.2 s, configure 8.0 s) | `results/phase-05/build-cost/linux-x86_64-container.txt` | MEASURED (1 run, container) |
| Meta-db memory | `docker stats` after the integration run | 39.8 MiB | phase report / ADR-0020 | MEASURED (1 sample) |
