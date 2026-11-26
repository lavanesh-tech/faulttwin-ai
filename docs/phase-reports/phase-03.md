# Phase 3 Implementation Report — PostgreSQL Monitoring

Date: 2026-11-26 · Prepared for independent technical review · Status: **In review, not complete.**

Terminology: *implemented* = code exists; *tested* = automated tests ran and passed;
*measured* = a metric was recorded under a protocol; *independently verified* = someone other than
the implementer reproduced it. In Phase 3 everything was built, tested and measured **by the AI
assistant in one Linux cloud container only**. Nothing ran on the owner's Mac or in GitHub Actions
yet, and nothing is independently verified.

Base: the owner's `main` at `f0624a4` (Phase 2, CI green). All Phase 3 changes are uncommitted in
the owner's working tree. The assistant ran no git command that changes anything in the owner's
repository.

---

## 1. Phase objective
Monitor a real replicated PostgreSQL cluster: a Docker Compose lab with a primary and two streaming
replicas (pinned version), a least-privilege read-only monitoring role, libpq collectors for
availability, connections, transaction throughput, query latency, WAL, replication state and lag,
replay positions, role and topology, Linux host resources, explicit separation of missing, stale
and invalid telemetry from healthy values, error handling, and integration tests including node
outages. Secrets live in `.env` with a committed `.env.example`. No detection, storage, API, chaos
or ML (later phases).

## 2. Business problem addressed
A monitor that reports a dead replica's lag as 0, or an idle replica's lag as eight minutes, sends
operators the wrong way during an incident. Phase 3 produces the first real telemetry and makes
every value say how much it can be trusted: a down node's metrics are `missing` (with the
connection error), never zeros; stale data is flagged with its age; malformed server output is
`invalid`; and metrics that do not apply to a role are `not_applicable`. The monitor runs with a
role that cannot write, read application data or change configuration, and a frozen server costs
one deadline instead of a hung collector. No business metric is improved yet; detection starts in
Phase 4.

## 3. Architecture decisions
- **ADR-0014 (new) Lab cluster:** PostgreSQL 17.12 pinned by multi-arch digest; async physical
  streaming with one slot per replica and `max_slot_wal_keep_size=1024MB`; `cluster_name` per node
  for edge matching; roles `ft_replicator`, `ft_monitor` (`pg_monitor` only, read-only defaults,
  timeouts, connection limit, no CREATE/TEMP, no application data), `ft_workload`; passwords only
  in `.env` and a `0600` passfile; ports on `127.0.0.1`; `faulttwin.role=lab-target` labels and
  `faulttwin.lab=on` for the Phase 6 guards.
- **ADR-0015 (new) Collector design:** libpq C API behind an RAII adapter and a small `SqlSession`
  interface (parsing is unit-tested with a fake); server- and client-side timeouts (async query +
  `poll` deadline; on expiry the connection is closed); one read-only REPEATABLE READ transaction
  per collection with `pg_stat_force_next_flush()` so the collector's own commit can be subtracted
  exactly; `Reading<T>` with five qualities and a reason; lag semantics chosen from lab
  observations (§10); derived topology with explicit issue codes; procfs host resources.
- **Module layout:** `faulttwin_collectors` (no libpq, always built) and `faulttwin_collectors_pg`
  (libpq adapter, built when CMake finds libpq; `FAULTTWIN_WITH_LIBPQ=AUTO|ON|OFF`; Homebrew's
  keg-only libpq is found automatically on macOS). `faulttwin-monitor` CLI in `src/monitor/`.
  This matches module 4 (`collectors`) of `docs/architecture/overview.md`; the `ICollector`
  start/sink interface sketched there is not built yet, because nothing consumes collector output
  in this phase (§23, question 1).
- **Not done on purpose:** Protobuf (ADR-0003 says "Phase 2–5"; there is no incident log yet, so it
  would be a schema without a user), per-container resources (needs privileges, §21).

## 4. Files created and modified
New (C++):
- `include/faulttwin/collectors/`: `reading.hpp` (quality model), `lsn.hpp`, `sql.hpp`
  (`SqlSession`, `SqlError`, SQLSTATE classification), `endpoint.hpp`, `pg_snapshot.hpp`
  (snapshot types, `collect_pg_node`, staleness, coverage), `rates.hpp`, `topology.hpp`,
  `linux_resources.hpp`, `libpq_session.hpp` (`LibpqSession`, `PgNodeCollector`).
- `src/collectors/`: `reading.cpp`, `lsn.cpp`, `sql.cpp`, `endpoint.cpp`, `pg_collector.cpp`,
  `rates.cpp`, `topology.cpp`, `linux_resources.cpp`, `pg/libpq_session.cpp`, `CMakeLists.txt`.
- `src/monitor/`: `main.cpp`, `monitor_options.{hpp,cpp}`, `monitor_report.{hpp,cpp}`,
  `CMakeLists.txt`.
- `tests/unit/collectors/`: `fake_sql_session.hpp`, `primitives_test.cpp`,
  `pg_collector_test.cpp`, `rates_topology_test.cpp`, `linux_resources_test.cpp`;
  `tests/unit/monitor_cli_test.cpp`; `tests/integration/lab_test.cpp`.

New (lab, scripts, build):
- `compose.yaml`, `.env.example`, `lab/primary/init/10-faulttwin-lab.sh`,
  `lab/replica/replica-entrypoint.sh`, `lab/workload/insert.sql`.
- `cmake/Libpq.cmake`; `scripts/lab-init-env.sh`, `scripts/run-integration-tests.sh`,
  `scripts/measure-collection.sh`, `scripts/summarize-collection.py`.
- `ci.phase-03.yml` (repository root; the owner moves it to `.github/workflows/ci.yml`).

New (docs, evidence): `docs/adr/0014-postgresql-lab-cluster.md`,
`docs/adr/0015-postgresql-collector-design.md`, this report, `results/phase-03/`.

Modified: `CMakeLists.txt` (include `Libpq`), `src/CMakeLists.txt`, `tests/CMakeLists.txt`,
`.gitignore` (`!.env.example`: the existing `.env.*` rule would have ignored it),
`scripts/verify-presets.sh` (default results directory `phase-03`), `README.md`,
`docs/PROGRESS.md`, `docs/architecture/acceptance-criteria.md`,
`docs/architecture/dependencies.md`, `docs/evaluation/metric-definitions.md`,
`docs/evaluation/limitations.md`, `docs/evaluation/verified-results.md`,
`docs/resume/evidence-ledger.md`.

## 5. Dependencies introduced and justification
- **libpq** (system package; 16.15 from Ubuntu 24.04 `libpq-dev` in the recorded runs; Homebrew
  `libpq` on macOS). The reference PostgreSQL client, a stable C ABI, PostgreSQL Licence. Not
  pinned: the server version is what matters, and it is pinned. Optional at build time.
- **PostgreSQL 17.12 server image** `postgres:17.12-bookworm@sha256:1eff563f…50d7` (lab only).
- **Docker Engine + Compose v2** for the lab (29.9.3 / v5.7.1 in the container; the owner's Mac has
  Docker 29.6.2 / Compose v5.3.1). Not needed to build or to run the unit tests.
- No new C++ library is linked into the libpq-free targets. libpqxx was rejected (ADR-0015).

## 6. Algorithms and data structures
- **Reading quality:** `Reading<T>` = quality + `optional<T>` + reason, invariant "value iff ok or
  stale"; `measured()` throws if called on a value-less reading. `mark_stale` changes only `ok`.
- **LSN:** `"XXXXXXXX/XXXXXXXX"` parsed with `std::from_chars` (1–8 hex digits per half, nothing
  else accepted) into a 64-bit offset; `bytes_between` refuses negative distances.
- **Collection:** 11–13 single-statement SELECTs per node inside one read-only transaction (§3).
  A failed statement rolls back and opens a new transaction. Each result is shape-checked (column
  count, row count) before parsing.
- **Rates:** per-second deltas over the steady-clock interval between two snapshots of the same
  node; counter decreases are `invalid`; the collector's own commits/rollbacks are subtracted;
  mean statement latency = Δtotal_exec_time / Δcalls (`missing` when Δcalls = 0).
- **Topology:** roles from `pg_is_in_recovery()`; one primary required (0 → `no_reachable_primary`,
  >1 → `multiple_primaries`); edges from the primary's `pg_stat_replication` matched by
  `application_name` = `cluster_name`; set differences give `replica_not_seen_by_primary` and
  `unknown_standby`; inactive slots give `slot_inactive`.
- **Host resources:** `/proc/stat` aggregate ticks (guest time excluded, already in user/nice),
  busy share between two samples; `/proc/meminfo` `(MemTotal − MemAvailable)/MemTotal`;
  `/proc/loadavg`; disk `used/(used+available)` like `df`.
- **Client timeout:** `PQsendQueryParams` + `poll(PQsocket)` until a monotonic deadline.

## 7. Time and space complexity
Per cycle and node: a constant number of statements; parsing is O(result size), i.e.
O(standbys + slots) on a primary, O(1) on a replica. Topology is O(N·S) for N nodes and S
standbys (N = 3 here). Memory per snapshot is O(standbys + slots) plus bounded strings (messages
≤ 240 bytes, names ≤ 64). The CLI keeps one previous snapshot per node. Measured client cost:
§16.

## 8. Memory ownership decisions
- `PGconn` and `PGresult` are owned by `std::unique_ptr` with `PQfinish`/`PQclear` deleters; no
  raw ownership. `LibpqSession` is non-copyable and non-movable (pImpl holds the connection).
- `PgNodeCollector` owns its session and drops it on any connection-level failure or deadline; the
  next cycle reconnects.
- Results are copied into `SqlTable` (strings) before the `PGresult` is freed, so snapshots never
  point into libpq memory. NULL is `std::nullopt`, not an empty string.
- The password is held in `PgCredentials` (in memory only) and passed as a libpq keyword; it is
  never placed in `argv` (the CLI refuses a `--password` option and reads `FT_MONITOR_PASSWORD`).

## 9. Thread-safety considerations
Phase 3 collection is single-threaded: the CLI collects nodes sequentially. A `PgNodeCollector` and
its `LibpqSession` must be used by one thread at a time (libpq connections are not shareable
between threads; separate connections are safe with the thread-safe libpq that distributions
ship). The collectors library has no global mutable state. `main` reads the environment before any
thread exists. Parallel per-node collection is a documented option for larger clusters
(ADR-0015), not built. The full suite still passes under TSan (§15); the libpq adapter is not
exercised concurrently anywhere.

## 10. Failure scenarios handled
| Scenario | Behaviour | Evidence |
|---|---|---|
| Node stopped (`docker compose stop`) | `down`, error `connection_failed`, every metric `missing` with the reason; primary shows 1 standby; topology `node_down` + `slot_inactive`; recovers to healthy after start | `StoppedReplicaIsDetectedAsDownAndRecovers` |
| Node frozen (`docker compose pause`) with an open connection | client deadline (1 s in the test) fires, connection closed, node `down` with `timeout`; reconnect fails by `connect_timeout` (2 s); no hang | `PausedReplicaTimesOutInsteadOfHanging` |
| Nothing listening / refused | `connection_failed`, single-line message | `RefusedConnectionIsAClassifiedError`, `CollectorReportsAnUnreachableNodeAsDown` |
| Permission denied on one view | node `degraded`; that view's metrics `missing` ("query failed: permission_denied"); the transaction is restarted and the rest is collected | `PermissionDeniedDegradesAndRestartsTheTransaction` |
| `pg_stat_statements` not installed | `missing` with that reason; no error, no aborted transaction | `MissingPgStatStatementsIsAQualityNotAnError` |
| Garbage from the server (negative counts, non-numbers, overflow, bad LSN) | `invalid` | `GarbageValuesAreInvalidNotZero` |
| Wrong result shape | `unexpected_result` error, metrics `missing` | `UnexpectedResultShapeIsReported` |
| Role unreadable | role `unknown`, role-specific metrics `missing`, no role-specific queries | `UnreadableRoleLeavesRoleUnknown` |
| Idle standby: `pg_stat_replication.*_lag` NULL | `missing` "no recent WAL activity", not 0 | `HealthyPrimary` |
| Idle primary: `now() − pg_last_xact_replay_timestamp()` grows | not used as lag (observed 474 s on a caught-up replica); exported as `last_replay_age_seconds` | `ReplicaWithPendingWalHasBacklogButNoTimeLag`, first lab run |
| Receiver restarted: receive LSN < replay LSN | backlog 0 "receiver restarted", replica caught up (observed after the stop test) | `ReceivePositionBehindReplayAfterReceiverRestartIsCaughtUp` |
| WAL receiver gone / not streaming | `receiver_status` missing, lag `missing` "not streaming", topology `replica_not_streaming` | `ReplicaWithoutWalReceiver`, `UnknownStandbyAndNotStreamingReplica` |
| Receiver silent > 30 s | backlog and lag `stale` (value kept) | `SilentReceiverMakesReceiveDerivedReadingsStale` |
| Snapshot older than `max_age` | every ok reading `stale`; missing stays missing | `StalenessIsSeparateFromMissing` |
| Stats reset / restart between snapshots | rate `invalid`, never negative | `CounterResetIsInvalidNotNegative` |
| Two primaries | `multiple_primaries`, no primary chosen | `TwoPrimariesAreASplitBrainRisk` |
| No procfs (macOS) / corrupt procfs | `missing` / `invalid` | `MissingProcfsIsMissingNotZero`, `CorruptProcStatIsInvalid` |
| Missing `.env`, unset password, bad node list | CLI exits 2 with a message | `MonitorOptionsTest.*`, `cli.monitor_requires_password` |

## 11. Security considerations
- **Least privilege, tested on the live server:** `ft_monitor` cannot create tables (read-only
  default → 25006; with the default lifted → 42501), create temporary tables, read
  `lab_app.orders`, call `pg_read_file`, or run `ALTER SYSTEM`; it is not superuser, cannot create
  roles or databases, cannot replicate, cannot bypass RLS, and has a 5-connection limit
  (`MonitoringRoleIsReadOnlyAndLeastPrivileged`). The collector also forces
  `default_transaction_read_only=on`, `statement_timeout` and `lock_timeout` per session.
- **Secrets:** only in `.env` (mode 600 from `lab-init-env.sh`, git-ignored; `.env.example` has
  placeholders). Never in `argv`, logs, JSON output or `postgresql.auto.conf` (the replica
  entrypoint refuses to start if a password lands there; verified `0` matches in the running
  replicas). Error messages are sanitized to one bounded line.
- **Exposure:** ports bound to `127.0.0.1`; replication allowed only from the Compose subnet.
- **Not done:** no TLS in the lab; the image trusts in-container local connections (used by the
  healthcheck and the measurement script). Both recorded in limitations.
- **Input handling:** every server value is parsed strictly (`from_chars`/`strtod` with end checks,
  finiteness, sign); strings are sanitized and length-bounded before they enter snapshots; JSON
  output escapes all control characters.

## 12. Actual build commands
Environment: Ubuntu 24.04.5 container, Linux 6.18 x86_64, 4 vCPUs, GCC 13.3.0, Clang 18.1.3,
CMake 3.28.3, Ninja 1.11.1, libpq 16.15, Docker 29.9.3, Compose v5.7.1.
```
scripts/verify-presets.sh gcc
scripts/verify-presets.sh clang
```
Each runs, per preset (debug, release, asan-ubsan, tsan):
`cmake --preset <p> -B build/<p>-<compiler> -DCMAKE_C_COMPILER=… -DCMAKE_CXX_COMPILER=…`,
`cmake --build build/<p>-<compiler>`, `ctest --test-dir build/<p>-<compiler> --output-on-failure`.
Lab:
```
scripts/lab-init-env.sh
docker compose up -d --wait
```

## 13. Actual test commands
```
scripts/verify-presets.sh gcc
scripts/verify-presets.sh clang
FAULTTWIN_RESULTS_LABEL=linux-x86_64-container-debug-gcc scripts/run-integration-tests.sh build/debug-gcc
FAULTTWIN_RESULTS_LABEL=linux-x86_64-container-asan-ubsan-gcc scripts/run-integration-tests.sh build/asan-ubsan-gcc
FAULTTWIN_RESULTS_LABEL=linux-x86_64-container-asan-ubsan-clang scripts/run-integration-tests.sh build/asan-ubsan-clang
FAULTTWIN_RESULTS_LABEL=linux-x86_64-container scripts/measure-collection.sh build/release 3
find include src tests -name '*.cpp' -o -name '*.hpp' | xargs clang-format-18 --dry-run --Werror
find src -name '*.cpp' | xargs clang-tidy-18 -p build/tidy --quiet --warnings-as-errors='*'
```

## 14. Test results
| Compiler | debug | release | asan-ubsan | tsan |
|---|---|---|---|---|
| GCC 13.3.0 | 179/179 pass | 180/180 pass | 179/179 pass | 179/179 pass |
| Clang 18.1.3 | 179/179 pass | 180/180 pass | 179/179 pass | 179/179 pass |

(Release has one more test, `bench.smoke`. In every preset the 5 lab tests are counted as passed
by CTest but report **Skipped** inside, because `FT_INTEGRATION` is unset.)

Integration tests with `FT_INTEGRATION=1` against the running lab:

| Build | Result | Log |
|---|---|---|
| GCC debug | 7/7 pass | `results/phase-03/integration/linux-x86_64-container-debug-gcc.log` |
| GCC asan-ubsan | 7/7 pass | `…/linux-x86_64-container-asan-ubsan-gcc.log` |
| Clang asan-ubsan | 7/7 pass | `…/linux-x86_64-container-asan-ubsan-clang.log` |

**macOS: NOT RUN. CI with `ci.phase-03.yml`: NOT RUN.**

New tests: 56 unit tests (Reading 4, Lsn 4, Sql 2, Endpoint 3, PgCollector 15, Rates 5,
Topology 7, LinuxResources 6, FakeProc 3, MonitorOptions 4, MonitorReport 3), 7 integration tests
(2 need only libpq and always run; 5 need the lab), 2 CLI tests. Without `FT_INTEGRATION=1` the 5
lab tests report **Skipped**, which is how the normal presets stay green without Docker.

clang-format: clean (exit 0) on `include src tests`. clang-tidy 18 with the committed config and
`--warnings-as-errors='*'`: clean (exit 0) on every `src/**/*.cpp`, including the libpq adapter
and the monitor. Getting there took refactoring (four functions over the cognitive-complexity
limit were split; reference members became pointers; optional accesses were made checkable;
the JSON writer was rewritten without escaped literals or `snprintf`).

## 15. Sanitizer results
ASan + UBSan and TSan, with the options from `CMakePresets.json` (`detect_leaks=1`,
`halt_on_error=1`, `detect_stack_use_after_return=1`, …): the whole unit and CLI suite passes with
no report under both compilers (`results/phase-03/{gcc,clang}/{asan-ubsan,tsan}.log`). The
integration tests, which exercise the real libpq adapter (connect, async query, poll deadline,
connection teardown on timeout, reconnect), also pass under ASan + UBSan with GCC and with Clang
against the live lab, with no leak or UB report. The integration tests were **not** run under
TSan (collection is single-threaded, §9). macOS sanitizers: NOT RUN.

## 16. Benchmark results
Phase 3 has no Google Benchmark suite; its measurements are the Module B monitoring metrics,
recorded by `scripts/measure-collection.sh` on the lab (Release build, GCC 13.3), in the container
described in §12, with the database, both replicas, the monitor and pgbench sharing 4 vCPUs.
Raw data: `results/phase-03/collection/linux-x86_64-container/`.

**A. Idle cluster, 300 cycles at 200 ms, 3 nodes (one run).**
- MON-SUCC 100 % (900/900 node collections). MON-COV 100 % (16,503/16,503 applicable readings ok).
- First cycle (opens 3 connections): 49.2 ms wall, 15.6 ms client CPU.

| Cycles 2–300 | mean | p50 | p95 | p99 | max |
|---|---|---|---|---|---|
| cycle wall ms (3 nodes, sequential) | 15.16 | 14.65 | 20.51 | 28.65 | 51.08 |
| per-node collection wall ms | 5.01 | 4.52 | 8.05 | 10.46 | 44.13 |
| client CPU ms per cycle | 1.73 | 1.69 | 2.26 | 2.57 | 2.95 |
| MON-FRESH: report time − collected_at, ms | 4.27 | 3.83 | 10.23 | 13.47 | 31.91 |

Server execution time of the monitor's own statements (pg_stat_statements, 300 cycles):
primary 0.359 ms/cycle (3,600 statements), replicas 0.233 and 0.209 ms/cycle (3,300 each).

**B. Throughput impact, pgbench 4 clients × 15 s, single-row inserts, 3 interleaved repetitions.**

| condition | tps per run | mean tps | vs none |
|---|---|---|---|
| no monitor | 5555, 5438, 5344 | 5446 | — |
| monitor every 1000 ms | 5462, 5450, 5261 | 5391 | −1.0 % |
| monitor every 100 ms | 5412, 5136, 5200 | 5249 | −3.6 % |

Run-to-run spread (max − min)/mean was 3.9 %, 3.7 % and 5.3 %. **Neither difference is
distinguishable from noise with three runs on a shared 4-vCPU host.** The honest reading is "no
measurable impact at 1 s; at 100 ms possibly a few percent, within noise".

**C. Outage timings from the integration tests (single runs each, not an experiment):**
| Run | stop: `compose stop` took | detected after it returned | healthy after `start` | pause: cycle with open connection | reconnect cycle | healthy after unpause |
|---|---|---|---|---|---|---|
| GCC debug | 359 ms | 9 ms | 510 ms | 1013 ms | 2010 ms | 25 ms |
| GCC asan-ubsan | 349 ms | 10 ms | 515 ms | 1015 ms | 2013 ms | 37 ms |
| Clang asan-ubsan | 365 ms | 13 ms | 510 ms | 1018 ms | 2017 ms | 39 ms |

The deadline and `connect_timeout` bound the paused cases exactly as designed (1 s and 2 s plus
~15 ms). Replicas replayed 200 committed inserts within 11–14 ms of the post-workload snapshot.

**E. Lab startup:** `docker compose up -d --wait` from empty volumes (image already pulled) took
11.36, 11.29 and 11.27 s.

**F. Found while running the integration tests back to back:** Docker marks a paused container
`unhealthy` at once, and it stays `unhealthy` until the first healthcheck after the unpause (about
2 s, reproduced by hand). `docker compose up --wait` fails immediately on `unhealthy`, so a second
run started right after the paused-replica test failed before running any test (twice, GCC and
Clang asan-ubsan). `scripts/run-integration-tests.sh` now retries `up --wait` up to 5 times, 3 s
apart; both reruns passed, and the Clang one needed the retry once.

**D. Observer effect found and fixed during the phase (measured before/after):** with one
autocommit statement per metric, the collector itself produced ~16 commits/s per node on an idle
cluster, which the first version reported as workload. After moving to one transaction per cycle,
forcing the stats flush and subtracting the collector's own commit, a 10-cycle idle run shows
0 commits/s in most intervals, and ~1 commit per interval about every 2 s, which is the Compose
healthcheck's `pg_isready` connection (6 commits in 10 s measured with psql alone on an idle
replica). That residue is real activity and is reported as such.

## 17. Baseline comparison
`docs/evaluation/baselines.md` defines baselines from Phase 4 on (rules vs ML, recovery policies);
Phase 3 has none to beat. The comparisons that exist are internal and honest:
- With vs without the monitor (§16 B): within noise.
- Naive vs chosen replica lag formula: the common `now() − pg_last_xact_replay_timestamp()`
  reported **474 s** for a replica whose apply backlog was 3.9 KB of checkpoint records and that was
  caught up one cycle later; the chosen rule reports 0 s when caught up and `missing` otherwise.
- Per-statement autocommit vs one transaction per cycle: ~16 vs ~0 phantom commits/s (§16 D).

## 18. Raw evidence artifact locations
- `results/phase-03/{gcc,clang}/`: `environment.txt` and full configure/build/test logs for each
  preset.
- `results/phase-03/integration/`: integration test logs with `FT_INTEGRATION=1` (environment
  header + full `ctest -V` output, including the `[ measured ]` lines).
- `results/phase-03/collection/linux-x86_64-container/`: `environment.txt`, `idle.jsonl.gz`
  (300 cycles), `idle-server.csv`, `pgbench.csv`, every pgbench console output, the monitor's JSON
  during each monitored pgbench run (`*.jsonl.gz`), and `summary.md`.
- `results/phase-03/README.md` explains the one annotation added to the environment files.

## 19. Updated evidence ledger
`docs/resume/evidence-ledger.md`: MON-SUCC, MON-COV, MON-OVH (client and server) and MON-FRESH are
`MEASURED (container only)`; MON-OVH throughput impact is `MEASURED`, with the explicit note that
it is within noise; MON-DETLAT is `NOT RUN` as an experiment (only integration-test timings). A
Phase 3 functional table lists the TESTED items. `docs/evaluation/verified-results.md` has the same
values, none independently verified.

## 20. Git status and commit SHA
- Base: `f0624a4` on `main`. The assistant ran **no** git command in the owner's repository.
  The container used a throw-away local git repository to track its own edits; its commit id
  `44a3ff8…` appears as `git_commit` in the container's `environment.txt` files and is **not** a
  commit of the owner's repository (annotated in each file; see `results/phase-03/README.md`).
- All Phase 3 files are uncommitted changes in the owner's working tree. The file tool cannot write
  into `.github/`, so the CI change is `ci.phase-03.yml` at the repository root: `libpq-dev`
  added to the build and clang-tidy jobs, log paths `phase-02` → `phase-03`, and a new
  `integration` job (builds debug, starts the lab, runs `scripts/run-integration-tests.sh`,
  uploads logs, always tears the lab down). Justification: the integration tests are this phase's
  main evidence and GitHub's Ubuntu runners have Docker.
- Copying files to the Mac does not keep the executable bit; the owner's steps include a `chmod`.
  `lab/replica/replica-entrypoint.sh` must be executable or the replicas will not start.
- No file names Claude, Anthropic or any AI as author or co-author.

Suggested commit message (for the owner to review and commit):
```
Phase 3: PostgreSQL monitoring

- lab: Docker Compose PostgreSQL 17.12 (digest-pinned) primary + 2
  async streaming replicas with physical slots; least-privilege
  read-only ft_monitor role; secrets in .env (.env.example committed)
- collectors: Reading<T> quality model (ok/missing/stale/invalid/
  not_applicable), LSN arithmetic, PostgreSQL snapshot collection in
  one read-only transaction, rates without observer commits, derived
  topology with issue codes, procfs host resources
- libpq adapter with client-side query deadlines and reconnect;
  faulttwin-monitor CLI (text / JSON lines)
- 56 unit tests, 7 integration tests incl. stopped and paused replica;
  all presets pass with GCC 13 and Clang 18; clang-tidy clean
- collection cost/freshness measurements under results/phase-03/
- ADR-0014, ADR-0015; docs, ledger, tracker updated; CI integration job
```

## 21. Known bugs and limitations
No known bugs. Limitations (also in `docs/evaluation/limitations.md`):
1. Everything ran in one 4-vCPU cloud container. macOS and GitHub Actions are NOT RUN for Phase 3.
2. Host resources are the collector host's, not each database container's.
3. Collector snapshots do not yet flow into the Phase 1 `TelemetrySample` or the Phase 2 pipeline
   (the model cannot express missing CPU/memory/disk).
4. Sequential collection: a down node adds up to `connect_timeout` (2 s) to the cycle.
5. No TLS in the lab.
6. libpq connection errors are classified from English message text.
7. Replica-side time lag is only "0 when caught up, otherwise missing"; time lag while WAL is
   pending comes only from the primary's `replay_lag` (NULL when idle).
8. The overhead experiment cannot resolve differences below ~5 % (shared host, 3 runs).
9. Outage timings are single runs measured from the `docker compose` command returning.
10. Healthcheck connections add ~0.5 commit/s per node of real but irrelevant workload.
11. After a pause, Docker reports the container unhealthy for ~2 s; the integration script retries
    `up --wait` to cope.

## 22. Incomplete requirements
- **NOT RUN:** the Phase 3 build and tests on the owner's Mac (Apple Silicon); the cluster on
  Docker Desktop for arm64; CI with `ci.phase-03.yml`. The owner's steps cover all three.
- **Not implemented:** per-container resource metrics; the `ICollector` streaming interface and
  feeding collector output into the ingest pipeline; Protobuf schemas (ADR-0003 says "Phase 2–5";
  nothing uses them yet).
- **Measured only partly:** MON-DETLAT (integration timings, not an experiment with ground truth).

## 23. Questions requiring architecture review
1. **Telemetry model:** `TelemetrySample` has non-optional CPU/memory/disk. Should Phase 4 change
   them to `Reading`-style optionals (my recommendation), or keep `TelemetrySample` as the
   synthetic/validated subset and add a richer `NodeObservation` for real collectors?
2. **Per-container resources:** read cgroups through the privileged `ft-faultctl` (Docker API), or
   run a small agent in each container (requires the gRPC path of ADR-0003)? I recommend the
   former for the lab and deferring agents.
3. **Lag source of truth:** is "primary's byte lag + primary's `replay_lag`, replica reports only
   caught-up" acceptable for the Phase 4 rules (e.g. "replica lag > 30 s")? The alternative is a
   heartbeat table written on the primary every second, which gives a true time lag at the cost of
   a write workload the monitor would own.
4. **Collection interval:** the defaults are 1 s and a 2 s query deadline. Phase 4 features use
   5 s windows (overview §5). Keep 1 s?
5. **CI integration job:** adds a Docker Hub pull (~170 MB) and about a minute per run. Run it on
   every push, or only on `main` and pull requests touching collectors?

## 24. Practical learning exercises
1. Replace the single transaction with one autocommit statement per metric and run
   `faulttwin-monitor --cycles 10` on an idle lab. Where do the commits come from, and why does
   `pg_stat_force_next_flush()` matter for subtracting them?
2. Compute replica lag as `now() - pg_last_xact_replay_timestamp()` and leave the lab idle for five
   minutes. Plot it next to `apply_backlog_bytes`. Which one would page someone at 3 a.m.?
3. `docker compose pause pg-replica-1` and run the monitor with `--query-timeout-ms 60000`. How long
   does a cycle take, and why does closing the connection beat `PQcancel` here?
4. Restart a replica and query `pg_last_wal_receive_lsn()` and `pg_last_wal_replay_lsn()` on it
   before any write. Explain why receive can be behind replay.
5. Grant `ft_monitor` `CREATE` on `public` and see which integration test fails. Then revoke it.
6. Stop a replica for ten minutes under pgbench load and watch `retained_bytes` on its slot. What
   protects the primary's disk, and what happens to the replica when the limit is hit?
7. Make `collect_pg_node` run the three nodes in parallel threads. What changes in ownership,
   and what would TSan have to say about `LinuxResourceCollector`?

## 25. Interview questions with explanations
1. **Why is missing lag not zero lag?** Zero means "measured and caught up"; missing means "we do
   not know". A dead replica reporting 0 lag looks healthy exactly when it is most dangerous.
   The type system enforces it here: a `Reading` has no value unless its quality is ok or stale.
2. **Stale vs invalid?** Stale is good data that is too old to act on (keep it, flag it); invalid
   is data that fails a check (negative count, garbage LSN) and must not be used at all.
3. **How do you measure replication lag in PostgreSQL?** Bytes: `pg_current_wal_lsn() −
   replay_lsn` on the primary. Time: `pg_stat_replication.replay_lag`, which is NULL when idle.
   The replay-timestamp age is not lag on an idle primary; a heartbeat table gives true time lag.
4. **Why one transaction per collection?** Consistent statistics snapshot across views
   (`stats_fetch_consistency = cache`), and a known, subtractable observer effect: one commit.
5. **How does the collector avoid hanging on a frozen server?** Server-side `statement_timeout`
   does not help when the server is frozen; the client sends asynchronously, polls the socket
   against a monotonic deadline and closes the connection when it passes.
6. **What does `pg_monitor` grant, and what does least privilege mean here?** Read access to
   statistics views and monitoring functions (`pg_read_all_stats`, `pg_read_all_settings`,
   `pg_stat_scan_tables`); no data, no DDL, no configuration, no signalling other backends. The
   role also defaults to read-only transactions, has timeouts and a connection limit, and the
   tests prove the denials on a live server.
7. **Why physical replication slots, and what is their risk?** They guarantee the primary keeps
   WAL a replica still needs; a dead replica can fill the primary's disk, so
   `max_slot_wal_keep_size` caps it and the collector reports inactive slots and retained bytes.
8. **Why does `pg_stat_replication` need `cluster_name`?** The walreceiver's `application_name`
   defaults to `walreceiver`; with `cluster_name` set, each replica is identifiable, so edges can
   be matched to inventory nodes.
9. **How would you show the monitor does not hurt the database?** Interleaved with/without runs
   under a fixed workload, enough repetitions to see the noise, server-side statement time from
   `pg_stat_statements`, and saying "within noise" when it is.

## 26. Phase acceptance criteria and current status
From `docs/architecture/acceptance-criteria.md` (Phase 3 criteria made explicit this phase):

| # | Criterion | Status |
|---|---|---|
| 3.1 | Compose lab: primary + 2 streaming replicas, version pinned | ✅ tested (Linux container) |
| 3.2 | Least-privilege read-only monitoring role | ✅ tested on the live server |
| 3.3 | Secrets only in `.env`; `.env.example` committed | ✅ implemented, checked |
| 3.4 | PostgreSQL collectors (availability … topology) | ✅ tested |
| 3.5 | Linux resource monitoring | ✅ tested |
| 3.6 | Missing / stale / invalid / not-applicable kept apart from healthy values | ✅ tested |
| 3.7 | Error handling | ✅ tested |
| 3.8 | Stopped replica detected and recovers | ✅ tested (Linux container) |
| 3.9 | Frozen replica costs one deadline, no hang | ✅ tested (Linux container) |
| 3.10 | Integration tests skip without Docker | ✅ tested |
| 3.11 | All presets pass with GCC and Clang; tidy and format clean | ✅ tested (Linux container) |
| 3.12 | Collection success, coverage, cost, freshness, throughput impact measured | ✅ measured (cloud container only) |
| 3.13 | Builds, tests and runs the lab on the owner's Mac | ⏳ NOT RUN |
| 3.14 | CI green with the Phase 3 workflow | ⏳ NOT RUN |
| 3.15 | No AI authorship; no assistant commits | ✅ |
| 3.16 | Owner approval after independent review | ⏳ |

**Phase 3 is not complete.** Open: the run on your Mac (3.13), CI with the new workflow (3.14), and
your review and approval (3.16). Phase 4 has not been started.
