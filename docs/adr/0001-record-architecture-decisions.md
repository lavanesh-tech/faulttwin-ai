# ADR-0001: Record architecture decisions

- Status: Proposed
- Date: 2026-11-25

## Context
FaultTwin AI is built over 30 phases. Decisions made early (process model, dependency management,
determinism) constrain later phases and must be explainable in reviews and interviews.

## Decision
Use lightweight ADRs (this format, `docs/adr/NNNN-title.md`). An ADR is written whenever a decision
has a credible alternative. ADRs are immutable once Accepted; changes create a new ADR that
supersedes the old one.

## Consequences
- Each phase report lists ADRs added or superseded.
- `docs/ARCHITECTURE.md` links ADRs instead of repeating their reasoning.
