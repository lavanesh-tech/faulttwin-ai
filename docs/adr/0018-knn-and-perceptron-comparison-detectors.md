# ADR-0018: k-NN distance and averaged-perceptron comparison detectors in Phase 4

- Status: Accepted (Phase 4)
- Date: 2026-11-26

## Context
The owner asked during Phase 4 for "K NN or any nearest neighbour and also perception
detection". The request was read as a k-nearest-neighbour anomaly detector and a
**perceptron** (a linear classifier). The owner was told about this reading and invited to
correct it.

ADR-0008 places the failure-prediction model in Phase 7. That model is trained in Python
(scikit-learn), selected empirically, exported to ONNX and run in C++. Phase 4 has no real
fault datasets (Phase 6) and no ONNX runtime. The question is how to add the two detectors
without pre-empting Phase 7 or leaking test data.

## Decision
1. **Both detectors are small native C++ comparison points** (`include/faulttwin/detection/
   learned.hpp`, rule-set version `faulttwin.learned.v1`). They are not the Phase 7 model, and
   ADR-0008 is unchanged.
2. **Same inputs and outputs as the rules.** Both read a fixed projection of 8 schema features:
   - `replication_lag_seconds.last` and `.slope_per_second`;
   - `cpu_percent.last` and `.mean`;
   - `statement_latency_ms.last`;
   - `connection_utilization.last`;
   - `availability.age_seconds` and `.missing_ratio`.

   A per-feature z-score is fitted on the training vectors. A missing value maps to 0 (the
   training mean), and z is clipped to ±10. The score goes through an alert state machine with
   the rules' semantics: fire after `sustain`, resolve below `clear` for 5 s, then a 30 s
   cooldown. The output is the same typed `Detection` (rule id `knn_distance` or `perceptron`),
   with evidence made of the score plus the three features that contribute most. Scoring uses
   the shared episode matching (ADR-0017 §6).
3. **k-NN distance (unsupervised).** The score is the mean squared Euclidean distance to the 5
   nearest reference vectors.
   - The reference set holds only *normal* training vectors: those outside every [onset,
     end + 30 s] window on their node. It is deterministically subsampled in data order to
     1,000 vectors.
   - The search is brute force, with an early exit once a partial distance exceeds the current
     k-th best. The early exit cannot change the result, and a unit test checks this against
     exhaustive search.
   - There is no square root, so the score uses only +, −, ×, ÷.
4. **Averaged perceptron (supervised).** Labels are +1 inside [onset, end] of an episode on the
   vector's node and −1 outside every [onset, end + 30 s]. The recovery tail is left out.
   - Training runs mistake-driven updates in fixed data order for 10 epochs. The returned
     weights are the average over all steps, which is more stable than the last iterate.
   - The score is w·z + b.
   - Classes are not reweighted. Fault vectors are about 5 % of the training set.
5. **No leakage.** Both detectors train on the first half of the validation seeds (1–20). They
   choose the alert trigger and sustain on the second half (21–40), using the rules' objective:
   F1, then fewer false alerts, then lower median latency.
   - The candidate triggers are quantiles (0.9 … 0.9995) of the scores of normal vectors on
     seeds 21–40. The candidate sustains are 0, 2, 5, 10 and 15 s.
   - The clear level is `trigger − 0.5 × (trigger − median normal score)`.
   - Each detector is evaluated once on the same test and shifted test splits as the rules.
   - The CLI requires at least 2 validation seeds, so that both halves are non-empty.
6. **Determinism.** The detectors use fixed data order, a deterministic subsample, no random
   initialisation and `-ffp-contract=off`. The CI evaluation step compares the two learned
   tuning CSVs and the alerts byte for byte with the committed results.

## Alternatives considered
- **Wait for Phase 7.** This would defer an explicit owner request. A native baseline costs
  little and gives Phase 7 another comparison point.
- **k-NN with a KD-tree or approximate search.** With 8 dimensions and 1,000 references, brute
  force with early exit costs microseconds per vector (see the phase report §14). An index adds
  code without need at this size.
- **Multi-class or one-vs-rest perceptrons, class weighting, non-linear features.** Any of
  these would likely raise the perceptron's recall. They were not added after the test split
  had been seen, because tuning the design on test results would be test-set peeking. The
  validation split already showed the weakness (F1 0.71 and recall 58 % on seeds 21–40).
  Phase 7 compares richer models properly.
- **Interpreting "perception" differently**, for example as a perception or attention model.
  The owner was asked. The perceptron is the closest common term.

## Consequences
- Phase 4 reports four detectors on the same splits: tuned rules, naive threshold, k-NN and
  perceptron. On this synthetic data the tuned rules remain best. The k-NN detector beats the
  naive threshold on precision and false alerts per hour, but is slower to alert. The single
  linear perceptron detects replication-lag and node-down faults and misses CPU saturation and
  overload entirely. All numbers are in `results/phase-04/detection/` and the phase report §12.
- The evaluation takes about twice as long (8.2 s instead of 3.7 s, GCC release).
- These results come from synthetic scenarios and say nothing about real PostgreSQL faults.
  Phase 6 data and Phase 7 models supersede them.
