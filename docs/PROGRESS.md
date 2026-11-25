# FaultTwin AI — Progress Tracker

Phases follow the owner's master specification. Statuses: Not started · In progress ·
In review (awaiting owner approval) · Complete. A phase is **Complete** only after its acceptance
criteria (`docs/architecture/acceptance-criteria.md`) are verified with saved output **and** the owner
approves it after technical review.

| # | Phase | Status | Notes |
|---|---|---|---|
| 0 | Architecture and evaluation design | In review | Docs in `docs/architecture/`, `docs/adr/`, `docs/evaluation/` |
| 1 | C++20 engineering foundation | In review | Committed as `3223488`. Linux: GCC + Clang, 4 presets pass. macOS debug passes. CI NOT RUN |
| 2 | Concurrent telemetry ingestion | Not started | Blocked on owner authorization |
| 3 | PostgreSQL monitoring | Not started | |
| 4 | Time-series features and anomaly detection | Not started | |
| 5 | Persistence, APIs and observability | Not started | |
| 6 | Chaos engineering and dataset generation | Not started | |
| 7 | ML training and native C++ inference | Not started | |
| 8 | Custom digital twin | Not started | |
| 9 | Recovery evaluation and ranking | Not started | |
| 10 | AI explanations and dashboard | Not started | |
| 11 | Performance optimization and hardening | Not started | |
| 12 | Portfolio, documentation and interview preparation | Not started | |

## Phase log

### 2026-11-25 — Phases 0 and 1 submitted for review
- Implemented: telemetry value model, validation contracts, freshness classification, deterministic
  synthetic generator, `faulttwin-synth` CLI, CMake presets, sanitizer builds, CI workflow, docs.
- Verified: 57/57 CTest tests pass for GCC 13.3 and Clang 18.1 in debug, release, asan-ubsan and
  tsan on Linux x86_64 (logs in `results/phase-01/`). clang-format and clang-tidy clean.
- Committed by the owner as root commit `3223488` (75 files). `ci.yml` not yet committed.
- Owner ran the macOS debug preset: AppleClang 21, 57/57 pass. Two warnings, both outside our own
  code: GoogleTest `-Wcharacter-conversion`, and an ld duplicate-library warning, now fixed
  (uncommitted).
- Not verified: GitHub Actions run, macOS release/sanitizer presets, line coverage.
- Report: `docs/phase-reports/phase-01.md`.
