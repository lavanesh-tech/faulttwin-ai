# ADR-0007: Deterministic simulation: integer time, total event order, specified RNG and distributions

- Status: Proposed
- Date: 2026-11-25

## Context
Reproducible simulation runs are a hard requirement. Common sources of nondeterminism: floating-point
time accumulation, ties in the event queue, iteration over unordered containers, thread scheduling,
and `<random>` distributions, whose algorithms are implementation-defined (libstdc++ and libc++
produce different sequences from the same engine and seed).

## Decision
- Simulation time is `int64_t` nanoseconds.
- Event key `(time, seq_no)` with a per-run monotonic `seq_no`.
- No iteration over unordered containers in model logic (or sorted before use).
- One run = one thread; parallelism only across runs; per-run seed derived by a fixed hash.
- PRNG: PCG64 or xoshiro256** implemented in-tree; distributions (uniform, exponential, normal via
  a fixed method, lognormal) implemented in-tree with golden-value tests.
- Each run emits a trace hash; determinism tests compare hashes.

## Consequences
- Cross-platform identical traces are expected for integer logic; floating-point model math may
  differ across compilers/flags (e.g. FMA contraction). We compile the sim library with
  `-ffp-contract=off` and verify; if cross-platform identity still fails, the guarantee is
  "identical on the same build", stated honestly.
