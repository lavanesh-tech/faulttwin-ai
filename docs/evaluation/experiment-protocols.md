# Experiment Protocols

Status: all experiments **PLANNED / NOT RUN**. Each protocol is fixed before its data is collected;
changes after seeing results are recorded with a reason.

## 1. Common requirements (every benchmark and experiment)

Each result artifact (`results/<phase>/…`, CSV or JSON) records: git commit SHA; execution date
(UTC); compiler and version; build type and optimization flags; OS and kernel; CPU model and core
count; RAM; container configuration (CPU/memory limits) if any; dataset/workload size; producer and
consumer counts; queue capacity; warm-up procedure; duration; repetition count; raw per-run values;
aggregation method (`metric-definitions.md` §1); failure, rejected and dropped-work counts; known
limitations.

- Performance runs use **Release** builds only. Sanitizer builds are for correctness only and are
  never compared with Release numbers.
- Each experiment has a script that reproduces the run and regenerates the report from raw data.
- Warm-up runs are discarded but logged. Machine background load is noted.

## 2. Experiments

### Experiment 1 — Concurrent ingestion (Phase 2)
Workload: ≥ 1,000,000 synthetic events per configuration from `faulttwin-synth`'s generator (fixed
seeds). Configurations: single-threaded baseline; producers {1,2,4} × consumers {1,2,4,8} with
queue capacity {1k, 16k}. 5 repetitions each after 1 warm-up. Metrics: ING-TPUT, ING-LAT-p50/p95/p99,
ING-CPU, ING-MEM, ING-REJ/DROP, ING-SCALE. Question: does concurrency increase *useful* processed
events under equivalent work?

**Phase 2 run (2026-11-25), recorded deviations.** (1) A per-event work parameter was added to the
grid: `work` = 0 (validation and ordering only) and 4096 extra SplitMix64 iterations in the sink,
the same in the baseline and the pipeline. Reason: decided before the recorded runs, after
exploratory runs showed that with near-zero work the result only measures queue hand-off cost.
(2) The pipeline uses the `kBlock` policy, so nothing is rejected or dropped. (3) Every repetition
is preceded by an untimed 100,000-event warm-up pass instead of one warm-up run per configuration.
(4) ING-CPU, ING-MEM and ING-SAT are not reported for Experiment 1. (5) Runs are on a 4-vCPU cloud
container, so the 4- and 8-consumer configurations oversubscribe the CPU.

### Experiment 2 — Overload resilience (Phase 2)
Arrival profiles: sustained 2× and 5× consumer capacity for 60 s; bursts of 10× for 1 s every 10 s;
recovery period after the spike. Compare baseline and bounded design under an identical memory cap.
Metrics: ING-MEM peak, latency, ING-ACC/REJ/DROP, time to drain after the spike, stability.

**Phase 2 run: reduced version only.** One producer offers 200,000 events as fast as it can to one
slow consumer (2,000 SplitMix64 iterations per event), once for each bounded policy (capacity 1024)
and once for the unbounded reference, each in its own process so that peak RSS (VmHWM) belongs to
one design. Not run: the 2× / 5× sustained and 10× burst arrival profiles, the 60 s duration, the
memory cap, latency and time-to-drain. The full protocol remains PLANNED.

### Experiment 3 — Rules vs ML (Phases 4, 7)
Data: independent scenario runs from Phase 6, split **by run** into train / validation / test;
overlapping windows from one run stay in one split; preprocessing fitted on train only; fault labels
and scenario IDs are never features; post-onset data excluded from pre-failure features. Tune rules
and model on validation; evaluate once on test. Metrics: DET-*. Report class balance, per-category
results and distribution-shift checks (held-out fault intensities).

**Phase 4 pre-check (synthetic, not Experiment 3).** Phase 6 data does not exist yet, so the rule
baseline was tuned and evaluated on deterministic synthetic scenarios (`faulttwin-detect
evaluate`): 900 s, 3 nodes, 1 s ticks, 3 fault episodes (replication lag build-up, CPU saturation,
overload, node down) and 5 benign distractors (short CPU/latency spikes, telemetry gaps, lag
blips, busy periods) per scenario. Split by seed: validation 1–40 (tuning only), test 1001–1040
and shifted test 2001–2040 (fault intensity 0.5–0.8 instead of 0.8–1.2), each evaluated once.
Comparison: the same rules without sustain, hysteresis and cooldown (naive thresholds, triggers
tuned the same way). Recorded deviations: the scenario generator was made harder once after a
first run showed near-perfect separation on every split, and its random-draw order was changed
once during refactoring (so one node's faults no longer shift another node's noise); the rule
features were switched from window means to last values after inspecting one validation scenario.
All three changes were made before the recorded run; the chosen parameters were identical before
and after the last two changes.

Learned comparison detectors (ADR-0018), added at the owner's request after the rule results
above had been recorded: a k-NN distance detector and an averaged perceptron on 8 projected
features. They are fitted on validation seeds 1–20 only. Their alert trigger (a quantile of
normal-vector scores) and sustain are chosen on validation seeds 21–40 with the rules' objective.
They are evaluated once on the same test splits. Their design was fixed before their first test
run and was not changed afterwards (the code was only refactored, and the results were checked
to be identical). The rules' tuning and results did not change.

### Experiment 4 — Recovery policy comparison (Phase 9)
Identical initial states, workloads and fault configurations per scenario; ≥ 10 runs per policy and
scenario; every executed strategy must pass the safety checks. Metrics: REC-*.

### Experiment 5 — Digital twin validation (Phase 8)
Calibration scenarios and evaluation scenarios are disjoint. Metrics: SIM-DT-AE/MAE/MAPE, SIM-BL-AE,
SIM-RANK, SIM-EPS; error distribution plotted, not only the mean. Whether MAPE < 15% is achievable is
an open question, not an assumption.

### Experiment 6 — Native inference (Phase 7)
Same ONNX file, same feature vectors (≥ 100,000), intra-op threads = 1, batch size 1 and 64, pinned
CPU frequency where possible. Metrics: DET-INF-LAT, throughput, RSS, CPU, parity (max abs diff).

### Experiment 7 — Incident reproducibility (Phases 8–9)
Replay every recorded incident ≥ 3 times; include incidents with deliberately missing or
version-mismatched artifacts. Metrics: RPL-*, ordering and ranking consistency.

### Experiment 8 — Safety evaluation (Phase 9)
Test cases: stale topology, missing telemetry, invalid predictions, unreachable replicas, excessive
backlog, split-brain risk, conflicting constraints, missing authorization, repeated approvals,
unsupported strategies. Metrics: SAFE-*, approval enforcement, audit completeness. A finite suite is
not proof of universal safety.

## 3. Phase 1 functional checks (actually run)

`faulttwin-synth` is a correctness demonstration, not a benchmark. Its CTest checks
(`cli.ground_truth_matches`, `cli.deterministic_output`) are recorded in the Phase 1 report.
