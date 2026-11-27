# Baselines

Every improvement is compared against a baseline that a competent engineer would accept as fair.
Baselines are never weakened to make the proposed design look better.

Status (Phase 2): Experiment 1's baseline is implemented (`faulttwin::ingest::run_single_threaded_baseline`,
same `EventProcessor` as the pipeline) and measured in a cloud container. Experiment 2's reference
(`faulttwin::concurrency::UnboundedQueue`) is implemented and measured in a reduced form, without
the memory cap. Experiment 3's rule baseline is implemented (Phase 4: `default_rule_set`, tuned by
`tune_default_rules` on validation scenarios) and evaluated on synthetic scenarios only, together
with a naive-threshold reference and two learned comparison detectors (k-NN distance and an
averaged perceptron, ADR-0018; native C++, not the Phase 7 model). The Phase 6 data and the
Phase 7 model are PLANNED. All other
baselines are PLANNED.

| Experiment | Baseline | Why it is fair | Proposed | Phase |
|---|---|---|---|---|
| 1 Concurrent ingestion | Single-threaded loop doing the same validate → process work, same input, same Release flags | Isolates the effect of concurrency; same work per event | Bounded-queue producer/consumer pipeline at several worker counts | 2 |
| 2 Overload resilience | Reference design with an unbounded `std::deque` + mutex queue, same consumers, run under a memory cap so it cannot harm the host | Represents the common "just queue it" design; capped so the test is safe | Bounded queue with explicit backpressure and rejection counting | 2 |
| 3 Rules vs ML | Rule baseline with thresholds, sustained-violation windows and hysteresis **tuned on the validation split** with the same objective as the model | Same data, same alert definition, same matching window, tuned with equal effort | Selected ML model via ONNX Runtime | 4, 7 |
| 4 Recovery policy | (a) "wait for automatic recovery" and (b) a fixed runbook policy: promote the most caught-up replica after N seconds of primary unavailability, with the same safety checks | Mirrors common HA policy; must satisfy the same safety constraints | Twin-assisted ranking with safety filter and ABSTAIN | 9 |
| 5 Twin validation | Naive predictor: historical median downtime per fault type from calibration scenarios | Simple, data-driven reference point | Calibrated twin prediction | 8 |
| 6 Native inference | Python `onnxruntime` with the same model file, feature vectors, thread settings and batch size | Same artifact and settings; only the host language differs | C++ ONNX Runtime | 7 |
| 7 Reproducibility | Re-running the live pipeline without recorded seeds/configuration | Shows what replay adds | Recorded incident replay | 8–9 |
| 8 Safety | Ranking without the safety filter (evaluated offline only, never executed) | Quantifies what the filter rejects | Safety filter + approvals | 9 |

Rules for every baseline:
- Same input data, workload, hardware, build type (Release) and repetition count as the proposal.
- Baseline code is reviewed with the same care as the proposal and kept in the repository.
- If the baseline wins, that result is reported.
