# Business-Impact Evidence Ledger

Purpose: every quantitative claim that may ever appear in the README, a resume or an interview must
have a row here with its evidence. **As of Phase 1 nothing has been measured, so no row may be used
in a resume.**

Statuses: `PLANNED` · `IMPLEMENTED` · `TESTED` · `MEASURED` · `INDEPENDENTLY VERIFIED` · `NOT RUN` ·
`FAILED`. A script or test existing does not make a metric `MEASURED`. `INDEPENDENTLY VERIFIED`
requires an actual independent reproduction.

Each row records: metric ID; business problem; engineering improvement; baseline value; improved
value; absolute difference; relative improvement; workload size; independent runs; environment;
command; commit SHA; raw artifact; status; limitations; potential resume wording.

| Metric ID | Business problem | Improvement | Baseline | Improved | Abs. diff | Rel. | Workload | Runs | Env | Command | Commit | Artifact | Status | Limitations | Possible wording |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ING-TPUT / REL-THR | Monitoring capacity | Concurrent bounded pipeline | — | — | — | — | — | — | — | — | — | — | PLANNED | — | template only |
| ING-LAT-p99 | Processing delay | Concurrent bounded pipeline | — | — | — | — | — | — | — | — | — | — | PLANNED | — | template only |
| ING-MEM / REL-MEM | Unbounded memory under overload | Bounded backpressure | — | — | — | — | — | — | — | — | — | — | PLANNED | — | template only |
| ING-SUCC | Lost work under overload | Explicit rejection accounting | — | — | — | — | — | — | — | — | — | — | PLANNED | — | template only |
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
| CI workflow green on GitHub | NOT RUN | — |
| Build and test on macOS (debug, AppleClang 21) | TESTED (owner-run) | `results/phase-01/macos/appleclang-debug.log` |

These are correctness facts about a small foundation. They are not performance or business
results and should not be phrased as such.
