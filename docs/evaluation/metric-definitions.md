# Metric Definitions

Defined **before** any experiment is run (master specification §11). No metric in this file has been
measured yet. Each metric has a stable ID used by `experiment-protocols.md`,
`verified-results.md` and `docs/resume/evidence-ledger.md`.

## 1. General rules

- **Aggregation across repeated runs.** Each experiment is repeated `n ≥ 5` times unless the
  protocol says otherwise. Report the median, the mean, the sample standard deviation and a 95%
  confidence interval (percentile bootstrap, 10,000 resamples, fixed seed) of the per-run values.
  Report every run in the raw artifact; never report only the best run.
- **Percentiles.** Latency percentiles are computed per run from all recorded samples (nearest-rank
  method on the sorted samples), then aggregated across runs as above. Percentiles are never averaged
  across runs as if they were means.
- **Relative vs absolute.** Always report the absolute baseline and improved values next to any
  relative change. Use "percentage points (pp)" for differences between two percentages, and
  "relative %" for ratios.
- **Zero denominators.** If a baseline value is 0 the relative change is **undefined** and is
  reported as `N/A (baseline = 0)` together with the absolute values. It is never reported as ∞ or
  as 100%.
- **Direction.** Each metric states whether higher or lower is better.
- **Environment.** Every value is tied to a commit SHA, build configuration and environment record
  (see `experiment-protocols.md` §1).

## 2. Relative-change formulas

| ID | Name | Formula | Better |
|---|---|---|---|
| REL-THR | Throughput improvement | ((improved − baseline) / baseline) × 100 | higher |
| REL-DT | Downtime reduction | ((baseline − improved) / baseline) × 100 | higher |
| REL-FA | False-alarm reduction | ((baseline rate − improved rate) / baseline rate) × 100 | higher |
| REL-MEM | Peak memory reduction | ((baseline peak − improved peak) / baseline peak) × 100 | higher |

## 3. Ingestion (Module A, Experiments 1–2)

| ID | Metric | Definition | Unit | Better |
|---|---|---|---|---|
| ING-ATT | Attempted events | events offered by producers | count | — |
| ING-ACC | Accepted events | events admitted to the queue | count | — |
| ING-PROC | Processed events | events fully handled by consumers | count | higher |
| ING-REJ | Rejected events | events refused at admission (backpressure/overload) | count | context |
| ING-DROP | Dropped events | admitted events later discarded (e.g. drop-oldest) | count | lower |
| ING-INV | Invalid events | events failing validation | count | — |
| ING-TPUT | Throughput | ING-PROC / measured wall-clock seconds (excluding warm-up) | events/s | higher |
| ING-LAT-p50/p95/p99 | End-to-end latency | consumer completion time − producer enqueue time, per event | ms | lower |
| ING-SAT | Queue saturation | fraction of sampling ticks with queue depth ≥ 90% capacity | ratio | lower |
| ING-CPU | CPU utilization | process CPU time / (wall time × cores available) | % | context |
| ING-MEM | Peak memory | peak resident set size (`VmHWM` from `/proc/self/status`) | MiB | lower |
| ING-SCALE | Scaling efficiency | TPUT(k workers) / (k × TPUT(1 worker)) | ratio | higher |
| ING-SUCC | Accepted-event processing success | ING-PROC / ING-ACC × 100 | % | higher |

Invariant checked in every run: `ATT = ACC + REJ` and `ACC = PROC + DROP + in-flight-at-stop`.

**Implementation mapping (Phase 2).** `faulttwin::ingest::IngestCounters` provides these counts:
ING-ATT = `attempted`; ING-ACC = `accepted`; ING-REJ = `rejected()` = `rejected_full +
rejected_timeout + rejected_cancelled + rejected_closed`; ING-DROP = `dropped_oldest`;
ING-PROC = `processed()` (every consumer-handled event, whatever its disposition); ING-INV =
`processing.count(kRejectedInvalid)`. "in-flight-at-stop" is split into `abandoned_at_shutdown`
(accepted, unprocessed at the drain deadline, handed back to the caller), `in_queue` and
`in_flight` (both 0 after `shutdown()`). The checked invariant is therefore
`accepted = processed + dropped_oldest + abandoned_at_shutdown + in_queue + in_flight`.
ING-LAT is measured from `submit()` (before the push, so it includes backpressure waiting) to the
end of the sink call. ING-SUCC uses `processing.delivered()` only if a run reports it as such;
Phase 2 reports `processed / accepted`. ING-SAT and ING-CPU are not reported in Phase 2 (queue depth
is not sampled over time; CPU time is in the raw JSON as `cpu_time`).

## 4. Monitoring (Module B)

| ID | Metric | Definition |
|---|---|---|
| MON-COV | Coverage | metrics collected / metrics defined for the topology |
| MON-SUCC | Collection success rate | successful collections / attempted collections × 100 |
| MON-OVH | Collection overhead | CPU and wall time per collection cycle on collector and database |
| MON-FRESH | Freshness | now − collected_at at consumption time (p50/p95/p99) |
| MON-DETLAT | Fault detection latency | first detection timestamp − ground-truth fault onset |

How Phase 3 measures them (`faulttwin-monitor`, `scripts/measure-collection.sh`):
- MON-SUCC: a collection attempt succeeds when the node was reachable (`up` or `degraded`).
  A node that is down and reported as down is a *correct* observation but not a successful
  collection.
- MON-COV: readings with quality `ok` / readings that apply to the node's role (everything except
  `not_applicable`), over the scalar readings of every node snapshot. Per-standby and per-slot
  readings and host resources are not counted.
- MON-OVH: client wall time per cycle and per node, client CPU per cycle (`getrusage` user +
  system), server execution time of the monitor's own statements (`pg_stat_statements`, filtered
  to the monitoring role), and the change in pgbench throughput with the monitor running.
- MON-FRESH: report time − `collected_at`, per node, at the point the report is written. In the
  CLI the consumer runs right after collection, so this is the collection-to-report age, not the
  age a later consumer (detection, Phase 4) would see.
- MON-DETLAT in Phase 3 is only the time from a `docker compose stop`/`pause` returning to the
  first cycle that reports the node down (integration tests). It is not Experiment-grade: there is
  no ground-truth onset timestamp inside the container.

## 5. Detection and prediction (Modules D–E, Experiment 3)

An **alert** is a rule firing or a prediction above the chosen threshold. Alerts are matched to
ground-truth failure episodes with the same matching window for rules and ML.

| ID | Metric | Definition |
|---|---|---|
| DET-P | Precision | TP / (TP + FP); N/A if no alerts |
| DET-R | Recall | TP / (TP + FN); N/A if no failure episodes |
| DET-F1 | F1 | 2PR / (P + R); N/A if P or R is N/A or P + R = 0 |
| DET-FPR | False positive rate | FP windows / negative windows |
| DET-FAH | False alerts per hour | FP alerts / hours of non-failure telemetry evaluated |
| DET-MISS | Missed failure rate | FN episodes / failure episodes |
| DET-LEAD | Prediction lead time | ground-truth failure onset − first qualifying prediction timestamp; only for episodes predicted *before* onset; abrupt failures are reported separately and never assigned artificial lead time |
| DET-INF-LAT | Inference latency | per-vector ONNX Runtime latency (p50/p99), Release build |
| DET-INF-OVH | Inference overhead | added CPU and RSS of the prediction path |

Per-category results (by failure type) and class imbalance are always reported.

**Implementation mapping (Phase 4, `faulttwin::detection::score`).** An alert is a `fired`
detection (escalations and resolutions are not alerts). An alert on node N at time t matches a
ground-truth episode on N when `onset <= t <= end + grace` (grace 30 s by default). DET-P is per
alert (matched alerts / alerts; several alerts in one episode are all matched); DET-R and
DET-MISS are per episode (episodes with at least one matched alert). DET-FAH = unmatched alerts /
node-hours outside every `[onset, end + grace]` window. Detection latency (reported next to DET-*,
and MON-DETLAT for live runs) = first matched alert − onset, per episode, nearest-rank p50/p95/max.
DET-FPR (window-based) is not computed in Phase 4: there are no labelled negative windows yet.
Undefined ratios are reported as N/A.

## 6. Digital twin and recovery (Modules F, H; Experiments 4–5)

| ID | Metric | Definition |
|---|---|---|
| SIM-EPS | Simulated events per second | events processed / wall seconds (Release) |
| SIM-DET | Determinism | identical trace hash across repeated runs (pass/fail, count) |
| SIM-DT-AE | Downtime absolute error | abs(predicted − observed downtime) per scenario |
| SIM-DT-MAE | Downtime MAE | mean of SIM-DT-AE over evaluation scenarios |
| SIM-DT-MAPE | Downtime MAPE | mean(abs(pred − obs) / obs) × 100 over scenarios with obs > 0; scenarios with obs = 0 excluded and counted |
| SIM-BL-AE | Backlog error | abs(predicted − observed replication backlog bytes) |
| SIM-RANK | Ranking agreement | Kendall's τ between predicted and observed strategy rankings |
| REC-DT | Observed recovery downtime | seconds without write availability |
| REC-SUCC | Recovery success rate | successful recoveries / attempted × 100 |
| REC-RTO / REC-RPO | RTO / RPO compliance | fraction of runs meeting the configured RTO / RPO |
| REC-LOSS | Transaction-loss exposure | committed-on-primary transactions not present after recovery |
| REC-UNSAFE | Unsafe recommendation rate | recommendations that violate a safety constraint / total |
| REC-LAT | Strategy evaluation latency | time from request to ranked result |

## 7. Replay, safety and explanations (Modules I–J; Experiments 7–8)

| ID | Metric | Definition |
|---|---|---|
| RPL-SUCC | Replay success rate | replays completing with identical required outputs / attempted |
| RPL-TIME | Replay runtime | wall time per replay |
| SAFE-REJ | Unsafe candidates rejected | count / total unsafe candidates in the test set |
| SAFE-FA | Incorrectly accepted | unsafe candidates accepted (target: 0, but a finite suite proves nothing universal) |
| SAFE-ABS | Abstention rate | ABSTAIN results / evaluations |
| LLM-CITE | Citation completeness | claims with valid evidence IDs / total claims |
| LLM-UNSUP | Unsupported numeric claims | numbers not matching cited evidence within tolerance |
| LLM-LAT / LLM-COST | Explanation latency / cost | per explanation |

## 8. Phase 1 validation counters (implemented)

These are produced by `faulttwin-synth` today. They are **functional self-checks**, not performance
or business metrics.

| ID | Metric | Definition |
|---|---|---|
| VAL-ATT | Attempted samples | samples generated |
| VAL-OK / VAL-BAD | Valid / invalid | samples accepted / rejected by `validate_sample` |
| VAL-GT-MISMATCH | Ground-truth mismatches | samples where the validator's verdict differs from the injected defect (must be 0) |
