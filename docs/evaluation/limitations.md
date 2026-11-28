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

## Phase 4
- **Detection quality is measured on synthetic scenarios only.** The generator is illustrative:
  stationary AR(1) noise, faults shaped by hand, no real PostgreSQL behaviour. The scenarios turned
  out to be easy to separate (tuned rules: 0 false alerts, F1 0.996 on the test split), so the
  numbers say what sustain, hysteresis and cooldown buy over naive thresholds, not how well real
  faults are detected. Experiment 3 needs the Phase 6 lab datasets.
- The generator was changed after results were seen (made harder once; draw order fixed once),
  before the recorded run. Tuning and test seeds never overlapped.
- Untuned design values: hysteresis clear ratio 0.8, clear_for 5 s, cooldown 30 s; the replication
  lag growth rule's level guard (1 s) and the latency rule's connection guard (60 %).
- Per-rule tuning against its own fault class ignores interactions between rules; the full rule
  set is only evaluated together on the test splits.
- Allowed lateness is 0: an observation older than its series' newest one is dropped (counted).
- Host CPU, memory and disk are not per database node in Phase 3 telemetry, so the CPU rule runs
  on synthetic data and on Phase 1 samples, not on the live lab.
- The live detection test covers one fault (stopped replica → `telemetry_stale`). Lag, CPU and
  overload rules have not fired against the real lab; that needs Phase 6 fault injection.
- The learned comparison detectors (k-NN, averaged perceptron) see the same synthetic data. A
  single linear perceptron with unweighted classes misses CPU saturation and overload entirely
  (0/34 and 0/35 on the test split). This was already visible on validation (recall 58 %) and is
  reported as is, not tuned away on test results. The k-NN reference set is a 1,000-vector
  subsample, and both detectors use untuned design values: k = 5, 10 epochs, hysteresis 0.5,
  and missing values mapped to the training mean.
- Feature definitions exist only in C++ so far; the Python implementation (Phase 7) must match
  the golden fixture.
- Cross-platform bit-identity of the scenarios rests on `-ffp-contract=off` and IEEE-754 doubles;
  it is tested on Linux x86_64 with GCC and Clang, and on Apple Silicon with AppleClang
  (`summary.json` byte-identical, owner-run).

## Phase 5
- Read endpoints have no authentication, and there is no TLS or rate limiting. `faulttwind`
  listens on loopback by default, and any other address needs `--allow-remote`. Phase 11.
- TLS is compiled out of Drogon's network library (`TRANTOR_USE_TLS=none`), so turning TLS on
  later needs a rebuild and an OpenSSL that Trantor supports (it fails against OpenSSL 4.x).
- One meta-db connection behind a mutex serves both API reads and recorder writes. This is fine
  at lab rates but was not measured under load. No API latency or throughput number exists yet.
- The recorder's retry queue is in memory. Detections queued during a meta-db outage are lost if
  the daemon is killed before the store returns. They are counted, and the next start closes
  orphans as `monitor_restarted`.
- The migration lock (advisory lock in one transaction) is designed for concurrent starts, but
  that race is not tested.
- `/api/v1/nodes` shows only the latest cycle. There is no history or trend endpoint yet.
- The synthetic source replays Phase 4 scenarios, so its values are generated, not measured.
- A Prometheus scrape of a loopback-bound `faulttwind` was verified only on Linux, through the
  docker0 address with `--allow-remote`. On Docker Desktop for macOS it is expected to work
  through `host.docker.internal`, but it was not verified.
- Grafana is AGPL-3.0. It runs as a separate, unmodified container only.
