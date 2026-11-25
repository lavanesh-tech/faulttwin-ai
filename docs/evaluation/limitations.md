# Known Limitations

Kept current with the implementation. Each phase adds its limitations here.

## Project-wide (by design)
- Results come from a small (three-node) Docker lab and synthetic workloads. They do not prove
  production readiness or commercial impact.
- The digital twin is a simplified model of PostgreSQL replication and recovery. It does not
  reproduce PostgreSQL internals.
- A finite safety test suite does not prove universal operational safety.

## Phase 1
- Only the telemetry value model, validation, a synthetic generator and a CLI exist. No ingestion,
  collection, storage, detection, ML, simulation, API or UI.
- Synthetic values are uniform random within fixed ranges. They are illustrative and do not model
  real load.
- Validation does not cap replication lag: a replica that is days behind is valid data, and the cap
  would hide real incidents.
- Timestamps are validated only against the epoch and a future-skew bound. Monotonicity per node and
  out-of-order handling belong to the ingestion pipeline (Phase 2).
- `ValidationPolicy::max_future_skew` is trusted configuration; extreme values that overflow
  `now + skew` are not guarded.
- Builds and tests ran on Linux x86_64 (GCC 13.3, Clang 18.1, all presets) and on macOS arm64
  (AppleClang 21, debug only). The GitHub Actions workflow has **not** run yet.
- Line coverage has not been measured.
