# ADR-0016: Event-time rolling-window features with Welford statistics and a versioned schema

- Status: Accepted (Phase 4)
- Date: 2026-11-26

## Context
FR-FEAT-1 asks for rolling windows, moving average, variance, rate of change, trend,
missing-value handling, event-time semantics, a versioned feature schema and deterministic
extraction. The same features feed the rules now and the Phase 7 model later, and a replay
(Phase 8–9) must reproduce them exactly. Phase 3 telemetry carries an explicit quality
(`ok`, `missing`, `stale`, `invalid`, `not_applicable`) that must survive into features.

## Decision
1. **Observation as the input unit.** `Observation{node, metric, event time, quality, value}`
   with a closed `MetricId` enum (10 metrics). Adapters map Phase 3 snapshots
   (`observations_from_cycle`) and Phase 1 samples (`observations_from_sample`); the extractor
   knows neither.
2. **Event time, allowed lateness 0.** Each `(node, metric)` series has a window over
   `(as_of − span, as_of]` keyed by the time the value was measured, never by arrival time.
   Observations must arrive in increasing event time per series; older or duplicate ones are
   refused and counted (`out_of_order`). Phase 2's ingest pipeline already orders per node, so
   a reorder buffer would add latency for no current producer. `extract(node, as_of)` with
   `as_of` earlier than the newest entry returns no values (counted) instead of silently mixing
   in "future" data.
3. **Fixed-capacity ring buffer per series** (default 256 entries, 60 s span). When full, the
   oldest entry is evicted even inside the span and counted (`capacity_evictions`). Memory is
   bounded by `max_nodes × metrics × capacity` (NFR-4).
4. **Welford for mean and variance**, with the inverse update on eviction, and an exact
   re-seed from the buffer every 1,024 evictions to bound the rounding error that removal
   accumulates. Test: a 200,000-push sliding run stays within 1e-9 relative of a two-pass
   recomputation; the textbook E[x²] − E[x]² formula fails a 1e9-offset case that Welford gets
   exactly. Min, max, rate (last − first over elapsed) and the least-squares slope (centred
   two-pass) are computed on read in O(n).
5. **Missing data is never a number.** Ok values feed the statistics; missing, stale and
   invalid entries are counted in `missing_ratio`; not-applicable values are ignored.
   `age_seconds` (time since the newest ok value ever seen, or since first sight when there
   never was one) makes staleness a feature. Any statistic that cannot be computed is
   `nullopt`, never 0.
6. **Versioned schema.** `FeatureSchema{version, window, ordered features}`; the default is
   `faulttwin.features.v1` (10 metrics × 9 kinds = 90 columns). Its FNV-1a fingerprint covers
   version, window and names in order and is pinned by a golden test; every `FeatureVector`
   carries version and fingerprint, and the rule engine refuses vectors of another schema.
7. **Determinism.** No wall clock, no hash-map iteration order (ordered `std::map`), no
   `<random>` distributions. `features`, `detection` are compiled with `-ffp-contract=off` so
   no compiler fuses a multiply-add (Apple Silicon does by default), which would change the last
   bits between platforms. Golden fixture: `scripts/features-reference.py` computes the expected
   values with exact rational arithmetic; the C++ results must match within 1e-9 relative.

## Alternatives considered
- **Processing-time windows**: simpler, but a replay or a slow collector would change features.
- **Reorder buffer with allowed lateness > 0**: needed once several unordered producers exist;
  deferred, the counter shows whether it is needed.
- **Recompute everything per read (O(n))**: simplest and exact, but makes push-time
  statistics unavailable; the incremental Welford state is what Phase 11 would optimise.
- **Kahan / pairwise summation**: no better than Welford for variance here, more code.

## Consequences
- Same observations and `as_of` → bit-identical vectors on one platform (tested), and within
  1e-9 relative of an exact reference everywhere (tested on Linux; macOS is owner-run).
- Late data is dropped, visibly. A producer that reorders must be fixed or a reorder buffer
  added (new ADR).
- Python (Phase 7) must implement the same definitions; the golden fixture is the contract.
