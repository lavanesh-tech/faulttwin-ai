# Phase 4 Implementation Report — Time-Series Features and Anomaly Detection

Date: 2026-11-26 · Prepared for independent technical review · Status: **In review, not complete.**

Terminology: *implemented* = code exists; *tested* = automated tests ran and passed;
*measured* = a metric was recorded under a protocol; *independently verified* = someone other
than the implementer reproduced it. In Phase 4 everything was built, tested and measured **by
the AI assistant in one Linux cloud container only**. Afterwards the owner ran the debug preset
and the integration tests on the Mac and CI ran green (§14). Nothing is independently verified. **All detection-quality numbers come
from synthetic scenarios.**

Base: the owner's `main` at `8f1dbb2` (Phase 3 approved). All Phase 4 changes are uncommitted in
the owner's working tree. The assistant ran no git command that changes anything in the owner's
repository.

---

## 1. Phase objective
The objective is to turn telemetry into features and features into alerts:
- event-time rolling windows (mean, variance, min, max, rate of change, trend, missing ratio,
  age) with numerically stable variance;
- a versioned feature schema with deterministic extraction;
- a rule baseline with thresholds, sustained violations, hysteresis, cooldown, multi-signal
  conditions, severity and stale-telemetry detection, tuned on validation data only.

During the phase the owner also asked for a **k-NN** and a **"perception"** detector. These were
built as a k-NN distance detector and an averaged **perceptron** (ADR-0018), and they are
evaluated alongside the rules. Phase 4 does not cover storage, APIs, chaos injection or the
Phase 7 model.

## 2. Business problem addressed
A threshold on a raw metric pages someone for every spike and stays silent when the collector
dies. Phase 4 alerts on sustained, multi-signal conditions:
- a missing value is not read as healthy or as violating, and a dead node is reported as stale
  telemetry;
- every alert says which rule set, rule, node, time and evidence produced it;
- thresholds are chosen on validation scenarios and judged once on held-out ones, so the
  reported quality is not tuned to the test.

On synthetic scenarios the tuned rules cut false alerts per node-hour from 1.86 (naive
thresholds) to 0.00 and raised recall from 91.7 % to 99.2 % (§12). Real-fault numbers need the
Phase 6 datasets.

## 3. Architecture decisions
- **ADR-0016 (new), features:**
  - `Observation` is the input unit, and each series has an event-time window over
    (as_of − span, as_of] with allowed lateness 0.
  - Each series has a fixed-capacity ring buffer.
  - Mean and variance use Welford with inverse updates on eviction and an exact re-seed every
    1,024 evictions.
  - Missing data is never a number.
  - The versioned schema `faulttwin.features.v1` has 90 columns and a pinned FNV-1a
    fingerprint.
  - `-ffp-contract=off` keeps results bit-identical across compilers.
- **ADR-0017 (new), rule baseline:**
  - A rule is predicates (trigger, clear and optional critical) plus a combiner, timing and
    severity.
  - Evaluation is three-valued: unknown ends a pending violation and holds a firing alert.
  - Each (rule, node) has its own state machine (pending, firing, recovering, cooldown).
  - The default set is `faulttwin.rules.v1`.
  - Tuning is per rule against its fault class on validation seeds 1–40. The test splits are
    evaluated once, and a naive-threshold baseline gets the same tuning effort.
- **ADR-0018 (new), learned comparison detectors:**
  - The k-NN distance detector is unsupervised: it uses normal vectors only, k = 5, and a
    1,000-vector reference set.
  - The averaged perceptron is supervised.
  - Both read 8 projected features, share the rules' alert state machine and scoring, train on
    validation seeds 1–20, choose thresholds on seeds 21–40 and run once on the test splits.
  - They are not the Phase 7 model, and ADR-0008 is unchanged.
- **Module layout:**
  - `faulttwin_features` depends only on collectors and telemetry types.
  - `faulttwin_detection` contains rules, scenarios, scoring, tuning and the learned detectors.
  - `faulttwin_detect_lib` and `faulttwin-detect` form the evaluation CLI.
  - `faulttwin-monitor --detect` provides online detection on the live lab.

## 4. Files created and modified
New (C++):
- `include/faulttwin/features/`: `observation.hpp`, `rolling_window.hpp`,
  `feature_schema.hpp`, `feature_extractor.hpp`, `adapters.hpp`
- `src/features/`: `rolling_window.cpp`, `feature_schema.cpp`, `feature_extractor.cpp`,
  `adapters.cpp`, `CMakeLists.txt`
- `include/faulttwin/detection/`: `rule.hpp`, `rule_engine.hpp`, `online_detector.hpp`,
  `scenario.hpp`, `evaluation.hpp`, `report.hpp`, `learned.hpp`
- `src/detection/`: `rule.cpp`, `rule_engine.cpp`, `online_detector.cpp`, `scenario.cpp`,
  `evaluation.cpp`, `report.cpp`, `learned.cpp`, `CMakeLists.txt`
- `src/detect/`: `detect_options.{hpp,cpp}`, `detect_report.{hpp,cpp}`, `main.cpp`,
  `CMakeLists.txt`
- `bench/detection_bench.cpp`

New (tests):
- `tests/unit/features/`: `rolling_window_test.cpp`, `feature_extractor_test.cpp`,
  `golden_features.inc`
- `tests/unit/detection/`: `rule_engine_test.cpp`, `scenario_evaluation_test.cpp`,
  `detect_options_test.cpp`, `online_detector_test.cpp`, `learned_test.cpp`
- `tests/cli/check_detect_deterministic.cmake`

New (scripts, docs, CI):
- scripts: `scripts/features-reference.py`, `scripts/run-detection-benchmarks.sh`
- ADRs: `docs/adr/0016-…`, `0017-…`, `0018-knn-and-perceptron-comparison-detectors.md`
- this report and `results/phase-04/` (README, logs, detection results, benchmarks)
- `ci.phase-04.yml` at the repository root (§20)

Modified:
- `src/CMakeLists.txt`, `bench/CMakeLists.txt`, `tests/CMakeLists.txt`
- `src/collectors/pg_collector.cpp`: `clock_timestamp()` fix (§10)
- `src/monitor/{CMakeLists.txt,main.cpp,monitor_options.hpp,monitor_options.cpp}` (`--detect`)
- `tests/integration/lab_test.cpp`: new detection test, plus a topology wait on a fresh lab
- `tests/unit/monitor_cli_test.cpp`
- `scripts/verify-presets.sh`, `scripts/run-integration-tests.sh` (default to `phase-04`)
- docs:
  - `README.md`, `docs/PROGRESS.md`;
  - `docs/architecture/{overview,requirements,acceptance-criteria,dependencies}.md`;
  - `docs/evaluation/{metric-definitions,experiment-protocols,baselines,limitations,verified-results}.md`;
  - `docs/resume/evidence-ledger.md`.

## 5. Dependencies introduced and justification
None. Google Benchmark (Phase 2) is reused for `bench/detection_bench.cpp`. The golden-fixture
script uses only Python's standard library (`fractions`). The k-NN and perceptron detectors are
about 200 lines of in-tree C++. A library such as mlpack or scikit-learn would add a dependency
for two trivial algorithms, and Phase 7 brings scikit-learn and ONNX Runtime for the real model.

## 6. Algorithms and data structures
- **RollingWindow:** a ring buffer of (time, quality, value) entries.
  - Welford add and remove for ok values, with an exact two-pass re-seed every 1,024
    evictions.
  - min, max, rate and least-squares slope are computed on read. The slope is centred and
    two-pass, which avoids cancellation.
  - `missing_ratio` = non-ok entries / applicable entries.
  - `age_seconds` = time since the newest ok value ever seen.
- **FeatureExtractor:** an ordered `std::map` of nodes, each holding one window per metric.
  `extract(node, as_of)` advances the windows and fills the schema's columns in order, using
  `nullopt` where no value is defined.
- **Adapters:**
  - A down node's availability is `missing`, timed at `attempted_at`.
  - Replica lag comes from the primary's standby row, matched by `application_name`. NULL lag
    with 0 bytes behind becomes 0 s.
  - Lag is `not_applicable` on the primary, and `missing` with zero or two primaries.
- **RuleEngine:** a state machine per (rule, node) with three-valued predicate logic (§3).
  Evidence lists every predicate's feature value.
- **Scenario generator:** SplitMix64 and Irwin–Hall normals over AR(1) signals. It places 3
  faults (lag build-up, CPU saturation, overload, node down) and 5 distractors (CPU spike,
  latency spike, telemetry gap, lag blip, busy period) per 900 s scenario. It uses no `<random>`
  and no libm, and every random value is drawn on every tick, so one node's fault never shifts
  another node's noise.
- **Scoring:** an alert on node N at time t matches an episode on N if onset ≤ t ≤ end + 30 s.
  - Precision is per alert and recall per episode.
  - False alerts per hour use node-hours outside every episode window.
  - Latency is measured from onset to the first matching alert, as a nearest-rank percentile.
- **Tuning:** a grid search per rule (trigger × sustain ∈ {0, 2, 5, 10, 15, 20} s). The
  objective is F1, then fewer false alerts, then lower median latency.
- **k-NN distance:**
  - z-scores are clipped to ±10, and missing values map to the training mean.
  - The score is the mean of the 5 smallest squared distances to the reference vectors.
  - The search is brute force with an early exit on a partial distance, which a unit test
    checks against exhaustive search.
- **Averaged perceptron:**
  - Mistake-driven updates run in fixed order for 10 epochs.
  - The averaged weights come from the standard accumulator trick (w − u / c).
  - The score is w·z + b.
- **ScoreAlerter:**
  - The trigger is a quantile of the scores of normal tuning vectors.
  - Clear = trigger − 0.5 × (trigger − median normal score).
  - Sustain, clear_for and cooldown work as in the rules.

## 7. Time and space complexity
- Push into a window costs amortised O(1), plus an O(n) re-seed every 1,024 evictions. Stats on
  read cost O(n) for min, max, slope and rate. A window uses O(capacity) memory, 256 entries by
  default.
- Extracting one vector costs O(metrics × n) per node. Memory is bounded by
  `max_nodes (64) × 10 metrics × capacity`.
- A rule evaluation costs O(predicates) per (rule, node), with O(rules × nodes) state.
- A k-NN score costs O(R × d) worst case, with R = 1,000 references and d = 8. The early exit
  makes it smaller in practice, and the model takes about 64 KB.
- A perceptron score costs O(d).
- `faulttwin-detect evaluate` with the default splits (100 scenarios evaluated, 40 used for
  tuning) took **8.2 s** wall time with GCC Release (§16). Before the learned detectors it took
  3.7 s.

## 8. Memory ownership decisions
All state is held by value in standard containers: windows in `std::vector` ring buffers, nodes
in `std::map`, models in `std::optional` members of `LearnedModel`. There is no raw ownership,
no shared ownership and no global mutable state, except the function-local `static` default
schema (built once and immutable). Detections and evidence are returned by value. Node counts
are capped (`max_nodes`, counted rejections), so a misbehaving producer cannot grow memory
without bound.

## 9. Thread-safety considerations
Feature extraction, rule evaluation and the learned detectors are single-threaded by design:
one extractor and one engine per evaluation thread, documented in the headers. Phase 2's
pipeline already serialises per node. `default_feature_schema()` and `learned_features()` are
immutable function-local statics, so initialisation is thread-safe in C++11 and later. The
whole suite passes under TSan with both compilers (§15). There are no multi-threaded detection
tests, because there is no concurrent detection code.

## 10. Failure scenarios handled
- **Out-of-order, duplicate or non-finite observations:** refused and counted. They never shift
  a window.
- **`extract` with an `as_of` earlier than the data:** no values, counted (`clock_behind_data`).
- **Vectors of another schema version, time going backwards, too many nodes:** refused by the
  engine, with a typed error.
- **Missing data:** rules see *unknown*. Stale telemetry is detected by its own rule, so a dead
  node is neither "healthy" nor "violating because 0 < threshold".
- **Invalid rule sets:** the `RuleEngine` constructor throws, so the error appears at startup.
- **Seed-range overlap between validation, test and shifted splits:** the CLI refuses it, and a
  test checks the refusal. The CLI also needs at least 2 validation seeds, one half for training
  and one for thresholds.
- **Online:** each node is evaluated as of its own snapshot time, so one slow node does not make
  the others look stale. This was a bug in the first version, found and fixed with a test.
- **Phase 3 bug found by Phase 4:** the replica's receiver-age query used `now()`, which is the
  transaction start, so the age came out negative and was reported `invalid` on a fresh lab. It
  now uses `clock_timestamp()`.

## 11. Security considerations
Phase 4 adds no network surface, credentials or privileged operations. `faulttwin-monitor
--detect` reuses the Phase 3 least-privilege role. `faulttwin-detect` reads no input files and
writes only to the `--out` directory the user names. The ground truth used in scoring is never a
feature (labels come only from the scenario's episode list, ADR-0018 §5). Lab secrets stay in
`.env`, which is not copied or committed.

## 12. Detection results (synthetic scenarios)
These are measured results from `results/phase-04/detection/linux-x86_64-container/summary.md`.
Validation seeds 1–40 (120 episodes) were used to tune the rules. The learned detectors trained
on seeds 1–20 and chose their thresholds on seeds 21–40.

Chosen parameters:

| Rule | Tuned trigger / sustain | Naive trigger |
|---|---|---|
| replication_lag_high | 5 s / 10 s | 15 s |
| replication_lag_growing | 0.1 s/s / 15 s | 0.25 s/s |
| cpu_saturation | 80 % / 15 s | 95 % |
| statement_latency_with_connection_pressure | 30 ms / 0 s | 30 ms |
| telemetry_stale | 1.5 s / 2 s | 3.5 s |

The learned detectors chose these alert settings:
- **k-NN:** trigger 7.519 (0.995 normal-score quantile), clear 3.794, sustain 10 s.
- **Perceptron:** trigger 2.356 (0.9995 quantile), clear −2.757, sustain 2 s.

**Test split** (seeds 1001–1040, intensity 0.8–1.2, 120 episodes):

| Detector | Alerts | False | Detected | Precision | Recall | F1 | False/h | p50 | p95 |
|---|---|---|---|---|---|---|---|---|---|
| tuned rules | 144 | 0 | 119/120 | 100.0 % | 99.2 % | 0.996 | 0.00 | 3 s | 33 s |
| naive threshold | 443 | 51 | 110/120 | 88.5 % | 91.7 % | 0.900 | 1.86 | 1 s | 30 s |
| k-NN distance | 114 | 6 | 108/120 | 94.7 % | 90.0 % | 0.923 | 0.22 | 16 s | 37 s |
| averaged perceptron | 57 | 8 | 49/120 | 86.0 % | 40.8 % | 0.554 | 0.29 | 11 s | 42 s |

**Shifted test split** (seeds 2001–2040, weaker faults, intensity 0.5–0.8):

| Detector | Alerts | False | Detected | Precision | Recall | F1 | False/h | p50 | p95 |
|---|---|---|---|---|---|---|---|---|---|
| tuned rules | 142 | 0 | 113/120 | 100.0 % | 94.2 % | 0.970 | 0.00 | 3 s | 30 s |
| naive threshold | 252 | 40 | 81/120 | 84.1 % | 67.5 % | 0.749 | 1.45 | 3 s | 39 s |
| k-NN distance | 108 | 5 | 103/120 | 95.4 % | 85.8 % | 0.904 | 0.18 | 22 s | 39 s |
| averaged perceptron | 68 | 8 | 60/120 | 88.2 % | 50.0 % | 0.638 | 0.29 | 5 s | 31 s |

Detected per fault class on the test split, with latency p50 in brackets:

| Detector | lag build-up | CPU saturation | overload | node down |
|---|---|---|---|---|
| tuned rules | 25/26 (21 s) | 34/34 (15 s) | 35/35 (0 s) | 25/25 (3 s) |
| naive threshold | 16/26 (24 s) | 34/34 (0 s) | 35/35 (0 s) | 25/25 (3 s) |
| k-NN distance | 19/26 (27 s) | 31/34 (33 s) | 34/35 (10 s) | 24/25 (14 s) |
| averaged perceptron | 25/26 (19 s) | 0/34 | 0/35 | 24/25 (4 s) |

Reading:
- **Rules vs naive thresholds:** the tuned rules remove all false alerts and recover recall on
  weak faults (shifted F1 0.970 vs 0.749). They pay a longer median latency for CPU saturation
  (the 15 s sustain).
- **k-NN:** it learns no labels, yet beats the naive thresholds on F1 and false alerts per hour
  on both splits. It is slower to alert (p50 16–22 s, because of the 10 s sustain), and it is
  weakest on lag build-up, whose early phase looks normal.
- **Perceptron:** one linear score over all four fault classes, with unweighted classes (5 %
  positives). It learned large weights for telemetry age and lag, and almost none for CPU (0.20)
  or latency (−0.95). As a result it misses every CPU saturation and overload episode. The
  validation split already showed this (F1 0.714, recall 58 %). The design was fixed before
  the test run and not changed afterwards, because tuning it on test results would be test-set
  peeking. Remedies (one-vs-rest, class weights, non-linear features) belong to the Phase 7
  model comparison.
- **The scenarios are easy for the rules,** with 0 false alerts on both splits. These numbers
  show what sustain, hysteresis and cooldown buy on illustrative data. They do not show
  real-fault quality (§21).

## 13. Actual build and test commands
Environment:
- Ubuntu 24.04 container, Linux 6.18 x86_64, 4 vCPUs;
- GCC 13.3.0, Clang 18.1.3, CMake 3.28.3, Ninja 1.11.1;
- libpq 16.15, Docker 29.9.3, Compose v5.7.1.

```
scripts/verify-presets.sh gcc
scripts/verify-presets.sh clang
FAULTTWIN_RESULTS_LABEL=linux-x86_64-container-debug-gcc scripts/run-integration-tests.sh build/debug-gcc
FAULTTWIN_RESULTS_LABEL=linux-x86_64-container-asan-ubsan-gcc scripts/run-integration-tests.sh build/asan-ubsan-gcc
FAULTTWIN_RESULTS_LABEL=linux-x86_64-container-asan-ubsan-clang scripts/run-integration-tests.sh build/asan-ubsan-clang
build/rel/src/detect/faulttwin-detect evaluate --out results/phase-04/detection/linux-x86_64-container
scripts/run-detection-benchmarks.sh gcc
scripts/run-detection-benchmarks.sh clang
find include src tests bench -name '*.cpp' -o -name '*.hpp' | xargs clang-format-18 --dry-run --Werror
find src -name '*.cpp' | xargs clang-tidy-18 -p build/tidy --quiet --warnings-as-errors='*'
```
`build/rel` is the release preset configured with g++.

## 14. Test results
| Compiler | debug | release | asan-ubsan | tsan |
|---|---|---|---|---|
| GCC 13.3.0 | 246/246 pass | 248/248 pass | 246/246 pass | 246/246 pass |
| Clang 18.1.3 | 246/246 pass | 248/248 pass | 246/246 pass | 246/246 pass |

Release has two more tests than the other presets: `bench.smoke` and `bench.detection_smoke`. In
every preset the 6 lab tests count as passed in CTest but report **Skipped** inside, because
`FT_INTEGRATION` is unset.

New since Phase 3 (179 → 246):
- 25 feature unit tests (windows 13, extractor, schema and adapters 12);
- 37 detection unit tests (rule engine 14, scenarios and scoring 10, CLI options 3, online
  detector 2, learned detectors 8);
- 4 CLI tests (help, overlapping splits refused, scenario JSON lines, deterministic output);
- 1 integration test.

Integration tests with `FT_INTEGRATION=1` against the running lab:

| Build | Result | Log |
|---|---|---|
| GCC debug | 8/8 pass | `results/phase-04/integration/linux-x86_64-container-debug-gcc.log` |
| GCC asan-ubsan | 8/8 pass | `…/linux-x86_64-container-asan-ubsan-gcc.log` |
| Clang asan-ubsan | 8/8 pass | `…/linux-x86_64-container-asan-ubsan-clang.log` |

In the new live test, `docker compose stop` was run on a replica, with 1 s cycles. Across the
three runs, `telemetry_stale` fired **3037, 3046 and 3043 ms** after the stop command returned.
The alert resolved **6097, 6125 and 6119 ms** after `start` was issued.

**Determinism:** the Clang Release build and the GCC Debug build were run with the default
arguments, and every output file was byte-identical (`cmp`) to the committed GCC Release
results. `cli.detect_deterministic` also checks that two identical runs match and that
different test seeds give different alerts.

**Lint:** clang-format (all of `include src tests bench`) and clang-tidy 18 with
`--warnings-as-errors='*'` (all of `src`) are clean. Reaching that took:
- splitting five functions that were over the cognitive-complexity limit;
- turning reference members into pointers;
- rewriting optional accesses so the checker can prove them;
- using raw string literals.

**macOS (owner-run, Apple Silicon, AppleClang, libpq 19.0, 2026-11-27):**
- Integration: 8/8 pass against the lab (`results/phase-04/integration/darwin-arm64.log`).
  `telemetry_stale` fired 3070 ms after the stop and resolved 6165 ms after the start.
- `faulttwin-detect evaluate`: `summary.json` was **byte-identical** to the Linux results, so
  the scenarios, features, rules and learned detectors give the same output on Apple Silicon.
- Debug ctest: the first run passed 245/246. `ScenarioTest.GoldenHashPinsCrossPlatformDeterminism`
  failed because the test hashed `time_since_epoch().count()`. `system_clock` ticks are
  nanoseconds in libstdc++ but microseconds in libc++, so identical timestamps gave different
  counts. This was a bug in the test, not in the generator; the identical `summary.json` shows
  the data matched. The test now hashes `duration_cast<nanoseconds>`. The golden value is
  unchanged on Linux, where GCC and Clang were re-run. After the fix the Mac debug ctest passed
  **246/246** (6 lab tests skipped inside).

CI: CI run #8 on `34879bf` green (6m 4s, seen in the owner's GitHub Actions screenshot), including the step that reproduces the committed detection results byte for byte.

## 15. Sanitizer results
ASan + UBSan and TSan ran with the options from `CMakePresets.json`. The whole unit and CLI
suite, including the learned detectors and the full scenario and tuning path, passes with no
report under both compilers (`results/phase-04/{gcc,clang}/{asan-ubsan,tsan}.log`). The
integration tests, including the online detection test, pass under ASan + UBSan with GCC and
with Clang against the live lab. They were not run under TSan, because detection is
single-threaded (§9). macOS sanitizers: NOT RUN.

## 16. Benchmark results
These come from Google Benchmark in Release, 5 repetitions per benchmark, in a 4-vCPU cloud
container (`results/phase-04/benchmarks/{gcc,clang}/`). The table shows medians, and the
coefficient of variation was 2–11 %.

| Benchmark | GCC 13.3 | Clang 18.1 |
|---|---|---|
| RollingWindow push, full window (60 / 256 entries) | 26.2 / 28.8 ns | 17.7 / 18.4 ns |
| RollingWindow stats on read (60 / 256 entries) | 0.46 / 1.69 µs | 0.31 / 1.30 µs |
| Feature vector extraction (90 features, one node) | 5.0 µs | 3.5 µs |
| Rule engine, 5 rules, one node | 143 ns | 141 ns |
| k-NN score (1,000 references, d = 8) | 11.9 µs | 11.2 µs |
| Perceptron score (d = 8) | 6.9 ns | 6.6 ns |
| End to end, one 900 s scenario (2,700 vectors, features and rules) | 12.3 ms (224k vectors/s) | 8.7 ms (314k vectors/s) |

At a 1 s collection interval with 3 nodes, the whole per-cycle cost (extraction, rules and
k-NN) is about 50 µs. Detection cost is not a constraint at this scale. k-NN scoring is the most
expensive step, at about 2.4× the cost of extracting the features.

## 17. Baseline comparison
`docs/evaluation/baselines.md` defines the Experiment 3 baseline as tuned rules versus the ML
model, on Phase 6 data. That experiment is not run yet. Phase 4 provides the synthetic
pre-check (§12): the tuned rules against a naive threshold with equal tuning effort, plus two
learned comparison detectors. The comparison is fair in the ways that matter:
- the same scenarios and feature vectors;
- the same alert matching and objective;
- tuning on validation only;
- one evaluation per test split.

## 18. Raw evidence artifact locations
- `results/phase-04/{gcc,clang}/`: environment and full logs for each preset.
- `results/phase-04/integration/`: integration test logs with `FT_INTEGRATION=1`.
- `results/phase-04/detection/linux-x86_64-container/`: the evaluation outputs:
  - `summary.json` and `summary.md`;
  - `tuning-{tuned,naive,knn,perceptron}.csv`;
  - `alerts-test.csv` and `alerts-test_shifted.csv`, which hold alerts from the tuned rules and
    both learned detectors (`rule_id` tells them apart);
  - `console.txt` and `environment.txt`.
- `results/phase-04/benchmarks/{gcc,clang}/`: `detection.json`, console output, environment.
- `results/phase-04/README.md` explains the notes appended to the environment files.

## 19. Updated evidence ledger
`docs/resume/evidence-ledger.md` has a Phase 4 functional table (TESTED items) and a table of
detection results, every row marked `MEASURED (synthetic)`. DET-F1 and DET-LEAD (Experiment 3)
stay `PLANNED`. `docs/evaluation/verified-results.md` has the same values plus the integration
timings and the benchmarks. None of it is independently verified.

## 20. Git status and commit SHA
- The base is `8f1dbb2` on `main`. The assistant ran **no** git command in the owner's
  repository.
  - The container tracked its own edits in a throw-away git repository. Its commit id
    `571a8e0…` appears as `git_commit` in the environment files. It is not the owner's
    repository, and each of those files carries an appended note saying so.
- All Phase 4 files are uncommitted changes in the owner's working tree.
- The file tool cannot write into `.github/`, so the CI change is `ci.phase-04.yml` at the
  repository root. It contains these changes:
  - log paths moved from `phase-03` to `phase-04`;
  - clang-format also checks `bench/`;
  - the integration job gains a step "Detection evaluation reproduces the committed results",
    which runs `faulttwin-detect evaluate` in the debug build and `cmp`s all 8 output files
    against `results/phase-04/detection/linux-x86_64-container/`.
- No file names Claude, Anthropic or any AI as author or co-author.

Suggested commit message (for the owner to review and commit):
```
Phase 4: time-series features and anomaly detection

- features: event-time rolling windows (ring buffer, Welford with
  periodic re-seed, min/max/rate/slope/missing ratio/age), versioned
  schema faulttwin.features.v1 with pinned fingerprint, adapters from
  collector snapshots and samples; golden fixture from exact arithmetic
- detection: rules with sustain, hysteresis, cooldown, three-valued
  missing-data logic, multi-signal conditions, escalation and stale
  telemetry; faulttwin.rules.v1 tuned on validation scenarios only
- deterministic synthetic scenarios with ground truth; scoring
  (precision, recall, F1, false alerts/h, latency); naive baseline
- k-NN distance and averaged perceptron comparison detectors
- faulttwin-detect CLI; faulttwin-monitor --detect on the live lab
- fix: replica receiver age uses clock_timestamp()
- tests, benchmarks, ADR-0016..0018, docs, evidence and CI updates
```

## 21. Known bugs and limitations
No known bugs. Limitations (also in `docs/evaluation/limitations.md`):
1. **Detection quality is measured on synthetic scenarios only.** The generator is
   illustrative, and the tuned rules reach 0 false alerts on it. Real faults come in Phase 6.
2. The generator was changed after its results were seen: it was made harder once, and its draw
   order was fixed once. The rule features were also switched from mean to last. All of this
   happened before the recorded run (`experiment-protocols.md`).
3. The perceptron misses CPU saturation and overload entirely (§12). This is reported as is.
4. Some design values were not tuned:
   - rules: hysteresis 0.8, clear_for 5 s, cooldown 30 s;
   - learned detectors: k = 5, 1,000 references, 10 epochs, hysteresis 0.5.
5. Per-rule tuning ignores interactions between rules.
6. Allowed lateness is 0, so late observations are dropped (counted).
7. CPU rules cannot run on the live lab, because the Phase 3 telemetry has no CPU per database
   node.
8. Only stale-telemetry detection was exercised live. Lag, CPU and overload rules need Phase 6
   fault injection.
9. Bit-identical evaluation output is tested on Linux x86_64 (GCC and Clang) and on Apple Silicon
   (AppleClang, `summary.json` only, owner-run).
10. Measurements come from one 4-vCPU cloud container. On macOS only the debug preset and the
    integration tests ran (owner). CI run #8 on `34879bf` is green.

## 22. Incomplete requirements
- **Done by the owner on the Mac:** debug 246/246, integration 8/8, identical evaluation output
  (4.11). macOS release and sanitizer presets: NOT RUN.
- **NOT RUN:** macOS release and sanitizer presets; real fault injection (Phase 6).
- **Not implemented:**
  - EWMA and median-MAD adaptive thresholds, deferred to Phases 6–7 (ADR-0017);
  - a reorder buffer for late data, deferred to an ADR if needed;
  - the Python feature implementation (Phase 7).
- **Measured only partly:** detection quality (synthetic only); live detection latency (single
  runs, stale telemetry only).

## 23. Questions requiring architecture review
1. **Perceptron follow-up.** Should Phase 7 include the k-NN and perceptron detectors as extra
   baselines in Experiment 3? Or should they stay a Phase 4 pre-check? I recommend keeping k-NN
   as a label-free baseline and dropping the single linear perceptron, because a one-vs-rest
   model is the fairer linear baseline.
2. **Rule features.** The rules use `last` values with a sustain period rather than window
   means. Is that acceptable, or should Phase 6 tuning search over both?
3. **CPU per database node.** This needs cgroup reads through the privileged fault controller
   (Phase 3 question 2) before the CPU rule can run on the lab.
4. **Allowed lateness 0.** Keep it until a producer actually reorders data?
5. **CI cost.** The evaluation step adds about 50 s (a debug-build run took 49 s in the
   container) to the integration job. Is that acceptable on every push?

## 24. Practical learning exercises
1. Replace Welford with E[x²] − E[x]² and run `RollingWindowTest`. Which test fails, and why
   does a 1e9 offset break it?
2. Set `sustain` to 0 for every rule and re-run `faulttwin-detect evaluate`. How do false alerts
   per hour change on the test split?
3. Add class weights to the perceptron (scale positive updates by n_neg / n_pos). Re-run on a
   *new* test seed range and explain why the old one must not be used.
4. Change k from 5 to 1 and to 25. Explain the change in false alerts per hour.
5. Remove the early exit from `KnnDetector::score` and compare the benchmark. Why is the result
   unchanged?
6. Stop a replica with `faulttwin-monitor --detect` running. Why does `telemetry_stale` fire
   about 3 s later and not at once?

## 25. Interview questions with explanations
1. **Why Welford instead of the sum of squares?** E[x²] − E[x]² subtracts two large, nearly
   equal numbers and loses all precision when the values have a large offset. Welford updates
   the mean and M2 incrementally with differences from the mean.
2. **What does hysteresis buy you?** A signal hovering at the threshold would fire and resolve
   repeatedly. A lower clear level plus `clear_for` turns that into one alert.
3. **How do you treat missing data in alerting?** Treat it as unknown, not as 0 and not as
   healthy. Unknown does not continue a pending violation or clear a firing alert, and a
   separate staleness rule reports the absence itself.
4. **How do you avoid tuning on the test set?** Split by scenario seed, tune only on validation,
   evaluate each test split once, and make the tool refuse overlapping splits. Here the learned
   detectors also split validation into a training half and a threshold half.
5. **Why can k-NN work as an anomaly detector without labels?** Normal behaviour fills a dense
   region of feature space, and a vector far from its nearest normal neighbours is unusual. The
   threshold is a quantile of normal scores, which sets the false-alert budget directly.
6. **Why did the perceptron miss whole fault classes?** It is one linear boundary for four
   different fault directions, trained with 5 % positives. The easiest-to-separate classes
   (telemetry age, lag) dominate the weights. One-vs-rest models or class weighting address it.
7. **How is the evaluation made reproducible across compilers?** It uses an in-tree PRNG, no
   libm in the generator, no hash-map iteration order and `-ffp-contract=off`. The output files
   are compared byte for byte in CI.

## 26. Phase acceptance criteria and current status
From `docs/architecture/acceptance-criteria.md` (Phase 4 criteria made explicit this phase):

| # | Criterion | Status |
|---|---|---|
| 4.1 | Event-time rolling windows, Welford variance, documented complexity | ✅ tested |
| 4.2 | Missing / stale / invalid / not-applicable handled explicitly | ✅ tested |
| 4.3 | Versioned schema, pinned fingerprint, deterministic extraction | ✅ tested |
| 4.4 | Rule baseline (sustain, hysteresis, cooldown, multi-signal, severity, stale) | ✅ tested |
| 4.5 | Tuned on validation only; test evaluated once; overlap refused | ✅ tested |
| 4.6 | Detection quality vs ground truth with a naive baseline | ✅ measured (synthetic only) |
| 4.7 | k-NN and perceptron comparison detectors without leakage | ✅ tested, measured (synthetic) |
| 4.8 | Online detection on the live lab | ✅ tested (Linux container) |
| 4.9 | All presets pass with GCC and Clang; tidy and format clean | ✅ tested (Linux container) |
| 4.10 | Benchmarks | ✅ measured (cloud container only) |
| 4.11 | Builds and tests on the owner's Mac | ✅ owner-run: debug 246/246, integration 8/8, evaluation byte-identical |
| 4.12 | CI green with the Phase 4 workflow | ✅ CI run #8 on `34879bf` green (6m 4s) |
| 4.13 | No AI authorship; no assistant commits | ✅ |
| 4.14 | Owner approval after independent review | ⏳ |

**Phase 4 is not complete.** Still open: your review and
approval (4.14). Phase 5 has not been started.
