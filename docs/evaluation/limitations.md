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
  (AppleClang 21, debug only). GitHub Actions
  (Ubuntu runner, GCC + Clang, 4 presets) has run green once, on `b36a903`. Its pinned actions use
  the deprecated Node.js 20 runtime and should be upgraded.
- Line coverage has not been measured.

## Phase 2
- All benchmark numbers come from one 4-vCPU Linux cloud container (Intel Xeon @ 2.10 GHz, shared
  host). They are not from the owner's machine, are not independently reproduced, and other
  tenants' load may affect them. Configurations with more than 4 threads oversubscribe the CPU.
- The workload is synthetic (Phase 1 generator) and the "work" per event is a CPU loop, not real
  enrichment or storage.
- With near-zero per-event work, the concurrent pipeline is slower than the single-threaded
  baseline: queue hand-off and cross-core cache traffic cost more than the work itself.
- Experiment 2 ran in a reduced form only (one arrival profile, no memory cap, no latency or
  drain-time measurement).
- Several consumers can process events of one node out of queue order, so out-of-order counts
  include pipeline-induced reordering. Duplicates older than the 64-sample window per node are
  reported as out-of-order.
- Shutdown cancellation is cooperative: a sink that blocks without watching its `stop_token` can
  delay shutdown indefinitely.
- No fairness between blocked producers.
- macOS (AppleClang/libc++) has not built Phase 2. `std::jthread` and `stop_token` waits need a
  recent libc++ (LLVM 18 or newer; AppleClang 21 should qualify, not verified).
- Peak RSS (VmHWM) is only available on Linux; the benchmark reports -1 elsewhere.

## Phase 3
- All Phase 3 runs (builds, tests, integration tests, measurements) were done by the AI assistant
  in one Linux x86_64 cloud container (4 vCPUs). Nothing ran on the owner's Mac or in GitHub
  Actions yet, and nothing is independently reproduced.
- The lab is one Docker host. Primary, replicas, the monitor and pgbench share the same 4 vCPUs, so
  the throughput-overhead experiment measures contention on one machine, not a networked
  deployment. Differences of a few percent are inside the run-to-run spread.
- Host resources are the collector host's (or container's) procfs, not each database container's.
  Per-container CPU/memory needs cgroup access or the Docker API, which the unprivileged core must
  not have (deferred to the privileged `ft-faultctl`, Phase 6). On macOS the procfs readings are
  `missing` by design.
- The Phase 1 `TelemetrySample` cannot represent missing CPU/memory/disk, so collector snapshots
  are not yet converted into it or fed through the Phase 2 pipeline.
- Collection is sequential across nodes; a down node costs up to `connect_timeout` (2 s) per cycle.
- No TLS in the lab; connections are plaintext on loopback and the Compose bridge.
- libpq connection errors are classified from English message text (no SQLSTATE is exposed).
- `pg_stat_statements` sums can decrease when entries are evicted (`pg_stat_statements.max`);
  the interval is then `invalid`, not a negative latency.
- The background commit rate of an idle node is not zero: the Compose healthcheck's `pg_isready`
  connection adds about one commit every 2 s per node, and it is reported, correctly, as workload.
- Outage detection times in the integration tests are measured from the `docker compose` command
  returning, not from a ground-truth fault onset; graceful `stop` closes connections at once, so
  the measured detection time is a lower bound for a crash.
