# ADR-0013: Google Benchmark for Phase 2 measurements

- Status: Accepted (Phase 2)
- Date: 2026-11-25

## Context
The master specification asks for Google Benchmark benchmarks in Release builds with
machine-readable output and full environment metadata.

## Decision
- Google Benchmark via `FetchContent`, pinned to the commit behind `v1.9.4`
  (`eddb0241389718a23a42db6af5f0164b6e0139af`), like GoogleTest (ADR-0004). Its own tests, install
  rules and `-Werror` are switched off. Licence Apache-2.0.
- Built only when `FAULTTWIN_BUILD_BENCHMARKS=ON`, which only the `release` preset sets. A
  non-Release build with benchmarks enabled prints a warning.
- `bench.smoke` (a CTest test in the release preset) runs the cheapest benchmark once to prove the
  binary works; it measures nothing.
- `scripts/run-benchmarks.sh` records the environment (`environment.json`), writes raw Google
  Benchmark JSON, and `scripts/summarize-benchmarks.py` (standard library only) produces
  `runs.csv`, `summary.csv` and `summary.md` with medians, means, standard deviations and 95%
  bootstrap CIs, as `docs/evaluation/metric-definitions.md` §1 requires.

## Alternatives considered
- Hand-written timing loops: no statistics, no standard JSON, easy to get wrong.
- nanobench: lighter, but the specification names Google Benchmark.

## Consequences
- The first Release configure fetches a second repository from github.com.
- Python 3 is needed to summarise results (not to build or test).
