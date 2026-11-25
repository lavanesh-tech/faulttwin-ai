# ADR-0010: Telemetry value model — raw input type, validated type, issue lists instead of exceptions

- Status: Accepted (Phase 1)
- Date: 2026-11-25

## Context
Telemetry arrives from collectors that can be wrong, partial or hostile (corrupted values, clock
drift, injected text). Downstream code (features, detection, simulation) must be able to trust a
sample without re-checking it, and validation must be deterministic so tests and replays are stable.

## Decision
1. **Two types.** `RawTelemetrySample` is a plain struct for untrusted input. `TelemetrySample` has
   no public constructor; only `validate_sample()` can create one ("parse, don't validate").
   `NodeId` is likewise valid by construction (`NodeId::parse`).
2. **Missing ≠ zero.** Replication lag is `std::optional<double>`. `nullopt` means "not measured".
   `ReplicationLagState` makes the three cases explicit: not applicable (primary/unknown role), not
   measured (replica without a value), measured.
3. **Role-specific semantics.** A primary or unknown-role node reporting any lag, even 0, is an
   error (`replication_lag_not_applicable`). For a corrupt role value, only finiteness and sign are
   checked, because role rules cannot be applied.
4. **Errors are values.** Validation returns all issues (at most one per field, in field order) and
   never throws for bad input. `std::expected` would fit but is C++23; the project targets C++20.
5. **Explicit timestamp policy.** Validation never reads a clock; the caller passes `now`.
   A timestamp at or before the Unix epoch is "unset"; more than `max_future_skew` (default 5 s)
   ahead of `now` is "in the future". Old samples are **valid but stale**: staleness is a separate
   classification (`classify_freshness`) so that "invalid" and "stale" are never conflated.
6. **Bounded identifiers.** 1–64 bytes, ASCII letters/digits plus `. _ -`, starting with a letter
   or digit. ASCII is checked explicitly instead of `std::isalnum`, which is locale-dependent and
   has undefined behaviour for negative `char` values.

## Alternatives considered
- Throwing exceptions from a validating constructor: hides control flow on a hot path and reports
  only the first problem.
- Sentinel values (e.g. lag = -1 for "missing"): exactly the confusion the requirements forbid.
- A single struct with an `is_valid` flag: nothing stops code from using an unvalidated sample.

## Consequences
- Every new field needs a check, a `Field` enumerator, a string form and tests.
- Strings for codes and fields are part of the output contract (CLI, later logs/API) and are tested.
