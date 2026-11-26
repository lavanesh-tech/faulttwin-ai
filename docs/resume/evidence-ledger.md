# Business-Impact Evidence Ledger

Purpose: every quantitative claim that may ever appear in the README, a resume or an interview must
have a row here with its evidence. **As of Phase 2 the only MEASURED rows come from one cloud
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
| Phase 2 build and tests on macOS | NOT RUN | |
| Phase 2 CI run | NOT RUN | |
