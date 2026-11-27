# ADR-0017: Rule baseline — sustained violations, hysteresis, cooldown, three-valued logic, tuned on validation only

- Status: Accepted (Phase 4)
- Date: 2026-11-26

## Context
FR-DET-1 asks for a deterministic rule baseline with thresholds, sustained violations,
hysteresis, cooldown, multi-signal conditions, severity and stale-telemetry detection, tuned on
validation data. The baseline is the comparison point for the Phase 7 model (Experiment 3), so
it must be tuned with real effort, not set up to lose. Phase 6 datasets (real lab faults with
ground truth) do not exist yet.

## Decision
1. **Rule = predicates on features + combiner + timing + severity.** A predicate compares one
   feature with a `trigger` and a `clear` level on the healthy side of it (hysteresis) and an
   optional `critical` level beyond it (escalation). Combiners `all` / `any`.
2. **Three-valued evaluation.** A predicate on a feature that is `nullopt` is *unknown*. Unknown
   ends a pending violation (the sustain period must be covered by data) and holds a firing
   alert (missing data neither confirms nor clears it). Staleness is detected by its own rule on
   `availability.age_seconds`, so a dead node is reported as stale, not as "healthy because no
   value exceeds a threshold" and not as "violating because 0 < threshold".
3. **Per (rule, node) state machine:** inactive → pending (violation) → firing after `sustain`
   → recovering (below `clear`) → resolved after `clear_for`, then `cooldown` during which a new
   violation cannot fire (counted as suppressed). Outputs are typed `Detection` events
   (`fired`, `escalated`, `resolved`) with rule-set version, rule id, node, severity, event time,
   violation start and the evidence values. Rule sets are validated before use (unknown feature,
   wrong-side hysteresis or critical, non-finite thresholds, negative durations, duplicate ids).
4. **Default rule set `faulttwin.rules.v1`:** replication lag high (with critical), replication
   lag growing (slope *and* level, multi-signal), CPU saturation, statement latency *with*
   connection pressure (multi-signal), telemetry stale (critical).
5. **Tuning protocol (pre-Experiment 3).** Until Phase 6, rules are tuned on deterministic
   synthetic scenarios with ground truth (`faulttwin-detect evaluate`): validation seeds 1–40,
   test seeds 1001–1040, and a shifted test set 2001–2040 with weaker faults. Each rule's trigger
   and sustain are chosen on validation only, one rule at a time against its own fault class:
   highest F1, then fewer false alerts, then lower median latency. The test sets are evaluated
   once with the chosen values. The CLI refuses overlapping seed ranges. A naive-threshold
   baseline (same predicates; no sustain, no hysteresis, no cooldown; triggers tuned the same
   way) is evaluated alongside.
6. **Alert matching** (shared with Phase 7): an alert on node N at time t matches an episode on
   N when onset ≤ t ≤ end + 30 s; unmatched alerts are false alerts; precision is per alert,
   recall per episode; false alerts per hour use node-hours outside every episode window.

## Alternatives considered
- **EWMA / median-MAD adaptive thresholds** (listed in overview §4): useful once real baselines
  vary per node; with synthetic stationary noise they add parameters without information.
  Deferred to Phase 6–7, where the comparison has real data.
- **Treating missing as violating or as healthy**: both turn a dead collector into a wrong
  answer; three-valued logic plus a stale rule keeps the distinction explicit.
- **Joint grid search over all rules**: exponential in rules; per-rule tuning against its own
  fault class is tractable and each choice is explainable.

## Consequences
- Synthetic results show what sustain and hysteresis buy over naive thresholds; they say
  nothing about real PostgreSQL faults (limitations L-15). Experiment 3 re-tunes on Phase 6
  data.
- Fixed design values that were not tuned: `clear_ratio` 0.8, `clear_for` 5 s, `cooldown` 30 s.
