# Phase 5 Implementation Report — Persistence, APIs and Observability

Date: 2026-11-28 · Prepared for independent technical review · Status: **In review, not complete.**

Terminology: *implemented* means code exists. *Tested* means automated tests ran and passed.
*Measured* means a metric was recorded under a protocol. *Independently verified* means someone
other than the implementer reproduced it.

In Phase 5, everything was built, tested and measured **by the AI assistant in one Linux cloud
container**, then built and tested by the owner on their Mac (debug 342/342, integration 25/25).
CI has not run yet (§22), and nothing is independently verified.

Base: the owner's `main` at `6209dee` (Phase 4 plus the Mac follow-up docs). All Phase 5 changes
are uncommitted in the owner's working tree. The assistant ran no git command that changes
anything in the owner's repository.

---

## 1. Phase objective
The master spec §19 gives the Phase 5 scope: "Incident storage. Versioned APIs. Drogon HTTP
service. Prometheus metrics. Structured logging. Remote gRPC telemetry transport where
justified."

Concretely, this phase turns the Phase 4 detections into **incidents** that persist outside the
monitored cluster. It serves them, plus the live node status, through a **versioned HTTP API** in
a long-running daemon (`faulttwind`). The daemon exposes its own health through **Prometheus
metrics** and **JSON logs**. gRPC was evaluated against the ADR-0003 condition and stays
deferred (ADR-0022).

Out of scope: fault injection (Phase 6), the ML model (Phase 7), the dashboard UI (Phase 10),
authentication for read endpoints, and TLS (Phase 11 hardening).

## 2. Business problem addressed
Before this phase, an alert existed only while `faulttwin-monitor --detect` was printing it.
After a restart nobody could answer basic questions: what fired, on which node, for how long,
whether it escalated, and whether someone was already looking at it.

Phase 5 addresses this in four ways:
- **Incident records survive restarts** and do not live on the database that is failing.
- **An API** lets operators and the later dashboard list, filter and acknowledge incidents.
- **Readiness and metrics** show when FaultTwin itself is degraded, for example when the store is
  down, cycles fail or writes queue up. A silent monitor is worse than no monitor.
- **Logs** that a machine can parse and that never contain credentials.

## 3. Architecture decisions
1. **ADR-0019: in-tree metrics and logging.**
   - Prometheus text 0.0.4 registry with a limit on label combinations per family and an
     overflow counter.
   - JSON-lines logger with secret redaction.
   - prometheus-cpp and spdlog were not adopted.
2. **ADR-0020: incident storage in a separate PostgreSQL meta-db.**
   - The meta-db has its own network, volume and least-privilege role.
   - Every operation is one SQL statement, with bind parameters only.
   - Migrations are versioned and run under an advisory lock.
   - A memory store implements the same contract.
3. **ADR-0021: Drogon v1.9.9 through pinned FetchContent.**
   - The HTTP rules live in a transport-free `ApiService`.
   - Drogon is a thin adapter.
   - vcpkg is not adopted, based on the measured build cost (§16). This settles ADR-0004.
4. **ADR-0022: gRPC stays deferred.**
   - No telemetry crosses a process boundary yet.
   - ADR-0003's Protobuf incident log is replaced by the meta-db.
5. **One process, three activities** (ADR-0002 modular monolith):
   - the monitor loop, on one `std::jthread`;
   - the incident recorder, called on the monitor thread;
   - the HTTP service, on Drogon's I/O threads.

   They share two thread-safe objects: the `IncidentStore` and a `StatusBoard`, which holds the
   latest snapshot behind a mutex.
6. **Safe defaults.**
   - The daemon listens on loopback.
   - Writes need a Bearer key of at least 16 characters. Without a key, writes answer 403 instead
     of being open.
   - Secrets come from the environment only.

## 4. Files created and modified
New (63 files):
- **Core**
  - `include/faulttwin/core/{json,time,logger}.hpp`, `src/core/*`: a strict bounded JSON parser
    and writer, ISO-8601 and epoch-microsecond conversion, and the structured logger.
- **Metrics:** `include/faulttwin/observability/metrics.hpp`, `src/observability/metrics.cpp`.
- **Storage**
  - `include/faulttwin/storage/{incident,incident_store,memory_incident_store,incident_recorder,pg_incident_store}.hpp`
    and `src/storage/{incident,memory_incident_store,incident_recorder}.cpp`.
  - `src/storage/pg/pg_incident_store.cpp`.
- **API:** `include/faulttwin/api/{http,status_board,api_service}.hpp`, `src/api/{http,api_service}.cpp`.
- **Daemon:** `src/server/{daemon_options,monitor_loop,drogon_adapter}.{hpp,cpp}`,
  `src/server/main.cpp` (`faulttwind`).
- **CMake:** `src/{core,observability,storage,api,server}/CMakeLists.txt` and `cmake/Drogon.cmake`.
- **Infrastructure**
  - `lab/metadb/init/10-faulttwin-metadb.sh` (executable): the meta-db role, database and
    schema.
  - `infra/prometheus/prometheus.yml`.
  - `infra/grafana/provisioning/{datasources,dashboards}/*.yml` and
    `infra/grafana/dashboards/faulttwin-self.json` (6 panels).
- **Scripts:** `scripts/e2e-faulttwind.sh` (executable): starts the daemon and checks it over real
  HTTP.
- **Tests**
  - `tests/unit/core/{json,time_logger,metrics}_test.cpp`.
  - `tests/unit/storage/{incident_store_contract.hpp,flaky_store.hpp,memory_store_test.cpp,recorder_test.cpp}`.
  - `tests/unit/api/api_service_test.cpp`, `tests/unit/server/daemon_options_test.cpp`.
  - `tests/integration/metadb_test.cpp`.
- **Docs**
  - `docs/adr/0019`–`0022`, `docs/api/openapi.yaml`, this report.
  - `ci.phase-05.yml`, at the repository root for the owner to move (§20).
- **Evidence:** `results/phase-05/…` (§18).

Modified:
- `include/faulttwin/collectors/sql.hpp`:
  - adds `query_params()`, a virtual with an error default, so the Phase 3 fakes are unchanged.
- `include/faulttwin/collectors/libpq_session.hpp`, `src/collectors/pg/libpq_session.cpp`:
  - parameterised queries through `PQsendQueryParams`; `query()` now delegates to them;
  - `ConnectOptions::read_only`, default **true**, so collectors stay read-only. Only the
    meta-db store turns it off.
- `src/detection/report.cpp`, `src/detection/CMakeLists.txt`:
  - the JSON and time helpers moved to `faulttwin_core` and are forwarded. The Phase 4 output is
    still byte-identical (§14).
- `CMakeLists.txt`, `src/CMakeLists.txt`, `tests/CMakeLists.txt`: new modules, Drogon, tests.
- `compose.yaml`:
  - the `meta-db` service, with network `meta` and port 55435;
  - `prometheus` and `grafana` behind the `observability` profile, digest-pinned.
- `.env.example`, `scripts/lab-init-env.sh`:
  - new keys: `FT_METADB_*`, `FT_API_KEY`, the Grafana admin password and ports;
  - re-running the script on an existing `.env` **appends missing keys only** and changes
    nothing else.
- `scripts/verify-presets.sh`, `scripts/run-integration-tests.sh`: default results phase
  `phase-05`.
- `README.md`; ADR-0003 and ADR-0004 status lines; the docs listed in §19.

## 5. Dependencies introduced and justification
| Dependency | Pin | Why | Licence |
|---|---|---|---|
| Drogon | v1.9.9, commit `38dd5fea31a7a2727c0a6f6b6b04252374796cab` (FetchContent, `SYSTEM`) | HTTP server named by the spec (ADR-0021) | MIT |
| Trantor | Drogon submodule at `8bf280ba043eb77c3eb02121065e0891a9a4f431` | Drogon's event loop | BSD-3-Clause |
| jsoncpp | system: Ubuntu `libjsoncpp-dev` 1.9.5; macOS `brew install jsoncpp` | Required by Drogon's headers. Our code uses its own parser | MIT / public domain |
| uuid, zlib | system: `uuid-dev`, `zlib1g-dev` on Ubuntu; macOS system libraries | Required by Drogon. OpenSSL is not used: Trantor is built with `TRANTOR_USE_TLS=none` (ADR-0021) | BSD / zlib |
| Prometheus image | `prom/prometheus:v3.5.0@sha256:63805ebb…` | Self-observability, opt-in profile | Apache-2.0 |
| Grafana image | `grafana/grafana:12.1.0@sha256:6ac590e7…` | Dashboard, opt-in profile | AGPL-3.0 (separate service, not linked) |

The meta-db reuses the Phase 3 `postgres:17.12` digest. Nothing new is linked into the Phase 1–4
libraries.

## 6. Algorithms and data structures
- **Incident lifecycle.** The recorder keeps a map from (rule_id, node) to the open incident id.
  - `fired` opens an incident.
  - `escalated` appends an event and raises the severity.
  - `resolved` appends the last event, closes the incident and removes the mapping.
  - At startup, `close_orphans()` resolves every incident still open as `monitor_restarted`,
    because no observation covers the gap.
- **Retry queue.** A bounded `std::deque` of detections (default 1024) is applied in order and
  stops at the first `kUnavailable`, so incidents are never reordered. When the queue is full, a
  new detection is dropped and counted. Validation failures (`kInvalid`) are counted and skipped,
  because retrying them cannot succeed.
- **PostgreSQL writes.** Each write is one statement using data-modifying CTEs. For example,
  resolve does three things in one statement: it updates the incident `WHERE state = 'open'`,
  inserts the event, and returns flags that distinguish "not found" from "already resolved".
  A unique partial index `(rule_id, node) WHERE state = 'open'` makes "at most one open
  incident" a database invariant.
- **Pagination.** Keyset pagination by id descending (`before_id`, `limit` ≤ 500). It is stable
  under concurrent inserts, which offset pagination is not.
- **Metrics.**
  - A family is a map from label values to a series, under a mutex taken only when a series is
    created. Callers keep references, so the hot path is lock-free atomics.
  - Histograms keep cumulative buckets plus a sum, and the count is derived from the buckets.
    Rendering sorts the series.
- **JSON parser.** Recursive descent with explicit limits: 64 KiB, depth 32, 16 KiB strings and
  4096 elements by default. The request body limits are tighter: 16 KiB and depth 4.
  - Duplicate keys are rejected, so `{"by":"a","by":"b"}` cannot smuggle in a second value.
  - UTF-8 is validated and surrogate pairs are decoded.
  - Numbers are checked against the JSON grammar before `strtod`.
- **Redaction.** One left-to-right scan that replaces values after `Bearer ` and after
  `<secret-like key>=`, both quoted and unquoted.

## 7. Time and space complexity
- Recorder per detection: O(log k) for the map, where k is the number of open incidents, plus one
  store call. Queue memory is O(max_pending).
- Memory store:
  - open, escalate, resolve and get: O(log n);
  - list: O(n) filter plus O(limit) output;
  - eviction: O(n) scan for the oldest resolved incident, only when full;
  - memory bounded by capacity × incident size, where an incident has at most 3 events and 64
    evidence items per event.
- PostgreSQL store:
  - list uses the primary key for the keyset; `opened_at` has its own index for the time
    filters, and the open-incident filter can use the unique partial index;
  - every call is one network round trip.
- Metrics:
  - increment: O(1), lock-free;
  - first use of a label set: O(log s) under the family mutex;
  - render: O(total series × buckets).
- JSON parse: O(input) time, with memory bounded by the limits.

## 8. Memory ownership decisions
- `main` owns, in declaration order: logger, registry, store, recorder, board, service,
  monitor loop. Everything else holds references or non-owning pointers to objects that outlive
  it.
- `MonitorLoop` declares its `std::jthread` last, so it is joined before the members it uses are
  destroyed.
- `PgIncidentStore` owns one `std::unique_ptr<SqlSession>`. It is reset on connection failure and
  recreated by the factory on the next call.
- `IncidentRecorder` holds pointers (not references) to the store and logger, so it is
  assignable in tests. Both outlive it by construction in `main`.
- Histogram buckets are a `unique_ptr<atomic<uint64_t>[]>`, because atomics cannot live in a
  growable vector.
- HTTP request and response structs are values. The adapter copies between Drogon and them,
  which costs one copy per request but keeps Drogon types out of the API layer.

## 9. Thread-safety considerations
- `ApiService::handle` is called from all Drogon I/O threads at once.
  - Its own state is atomics plus the registry.
  - The store is internally synchronised: a `shared_mutex` in memory, a mutex around the single
    PostgreSQL connection.
  - `StatusBoard` copies a snapshot under a mutex.
- The recorder is used only from the monitor thread, plus the final `flush()` after that thread
  is joined. It is documented as not thread-safe.
- Signals: `main` blocks SIGTERM and SIGINT before any thread starts, so every thread inherits
  the mask. One dedicated thread takes them with `sigwait`. Shutdown therefore runs as ordinary
  code, never inside a signal handler. Drogon's own handler is disabled
  (`disableSigtermHandling`), see §15.
- Shutdown order:
  1. The signal thread sets draining, so `/readyz` answers 503.
  2. Drogon's loop quits and `app.run()` returns.
  3. `loop.stop()` requests stop, wakes the sleep, joins the thread and flushes the recorder.
  4. Exit.
- The monitor sleep is `condition_variable_any::wait_until(lock, stop_token, …)`, so stopping
  never waits a full interval.
- ThreadSanitizer: the `tsan` preset runs every unit test, including concurrent API calls and
  four concurrent store writers. §15 has the results. The end-to-end daemon test runs in every
  preset, and so under TSan too.

## 10. Failure scenarios handled
| Scenario | Behaviour | Test |
|---|---|---|
| Meta-db down at startup | `migrate()` fails, so exit 3 with a JSON log line. There is no half-started server | manual run, plus the `make_store` code path |
| Meta-db down while running | Writes return `kUnavailable`. Detections queue in order (bounded), `/readyz` answers 503 with store detail, reads answer 503 with `Retry-After: 5`. Recovery replays the queue in order | `RecorderTest.KeepsOrderAcrossAnOutageAndRetries`, `ApiTest.StoreOutageIs503WithRetryAfter`, `ApiTest.ReadyzNeedsAMonitorCycleAndAStore` (FlakyStore) |
| Retry queue full | New detections are dropped and counted (`faulttwin_incident_detections_dropped_total`) | `RecorderTest.BoundedQueueDropsAndCountsInsteadOfGrowing` |
| Meta-db schema newer than the build | Refuses to start (`kInvalid`) | `integration.MetaDb.RefusesASchemaNewerThanThisBuild` |
| Two processes migrating at once | Advisory lock inside one transaction | migration design. Idempotence is tested; the race itself is not |
| Duplicate open incident (bug or race) | Unique partial index, so `kConflict` | `integration.MetaDb.OneOpenIncidentPerRuleAndNode…` and the contract |
| Daemon killed with open incidents | Closed as `monitor_restarted` on the next start | contract `CloseOrphans…`, observed in the postgres e2e run |
| Monitored node down or paused | Phase 3 collector deadlines; node shows `down`; `telemetry_stale` fires | Phase 3/4 integration tests and the live run (§14) |
| Malformed, oversized, deep or duplicate-key JSON | 400 or 413 before any store call | `JsonParse.*`, `ApiTest.AcknowledgeValidatesTheBody` |
| Unknown or repeated query parameter | 400 (never ignored) | `ApiTest.RejectsBadListParameters` |
| Wrong method, unknown version | 405 with `Allow`; 404 `unsupported_api_version` | `ApiTest.*`, e2e |
| Label explosion in metrics | Sample goes to the overflow series and is counted | `Metrics.CardinalityIsBoundedAndOverflowIsCounted` |
| SIGTERM during traffic | Drains, joins, flushes, exit 0 | e2e script (`wait` checks the exit code) |

## 11. Security considerations
- **Secrets.**
  - `FT_API_KEY`, `FT_MONITOR_PASSWORD` and `FT_METADB_PASSWORD` are read from the environment
    only, never from argv, so they never appear in `ps`.
  - They are never logged. Secret-like keys and `Bearer`/`key=value` text are redacted in every
    log record, and a unit test covers a libpq-style error string.
  - `.env` is generated with random values and git-ignored.
- **Authentication.**
  - The one state-changing endpoint needs `Authorization: Bearer <key>`, compared in constant
    time.
  - With no key configured, it returns 403 `writes_disabled`, so writes are not open.
  - The key must be at least 16 characters, otherwise startup refuses.
- **Exposure.**
  - The default listen address is `127.0.0.1`. Anything else needs `--allow-remote`, because
    read endpoints are unauthenticated in Phase 5 (stated in the OpenAPI file).
  - Compose publishes the meta-db, Prometheus and Grafana on 127.0.0.1 only.
- **Injection.**
  - All SQL values are bind parameters.
  - A contract test stores `r'; DROP TABLE faulttwin.incidents; --` and quotes, backslashes and
    UTF-8, and reads them back verbatim.
- **Least privilege.** `ft_app`:
  - has no superuser, `CREATEDB`, `CREATEROLE`, replication or `BYPASSRLS`;
  - has a connection limit of 10;
  - has `statement_timeout`, `lock_timeout` and `idle_in_transaction_session_timeout` set;
  - can create objects only in its own schema.

  An integration test checks the role flags, and checks that `CREATE DATABASE`, `CREATE ROLE`
  and `CREATE TABLE public.x` fail. The collectors' sessions stay read-only, which is the
  default.
- **Isolation.** The meta-db is on its own Docker network. Phase 6 fault injection on the `lab`
  network cannot reach it. Its containers carry the label `faulttwin.role: observer`, for the
  Phase 6 guards.
- **Responses.**
  - Every response has `Cache-Control: no-store` and `nosniff`, and the `Server` header is off.
  - Internal store errors return a generic 500 while the detail goes to the log.
  - Request ids from clients are accepted only if they match `[A-Za-z0-9._-]{1,64}`, so a client
    cannot inject text into logs.
- **Not done:** TLS, authentication for reads, rate limiting. These are Phase 11, and are listed
  in limitations.

## 12. Results of the new functionality
- **Live lab** (`--source postgres --store postgres`, 1 s cycles, Linux container):
  - `/readyz` reported the store reachable and the last cycle 975 ms old;
  - `/api/v1/nodes` showed 3 nodes up with the correct roles;
  - replica lag was 0.69–0.76 ms and probe latency 0.15–0.43 ms;
  - after 4 cycles, `faulttwin_monitor_cycle_errors_total` was 0;
  - SIGTERM gave exit 0;
  - on start, the daemon logged that it closed 1 incident left open by the previous run
    (`monitor_restarted`).

  These are single-run observations from a terminal session, not saved as an artifact. The
  integration and e2e tests below are the reproducible evidence.
- **Synthetic source** (Phase 4 scenario replay): incidents open, escalate and resolve. Acknowledge
  returns 200 once and 409 the second time (e2e). With `--store postgres`, the meta-db held the
  daemon's incident with its acknowledgement after exit.
- **Self-observability**, saved in `results/phase-05/observability/linux-x86_64-container.txt`:
  - Prometheus 3.5.0 scraped `faulttwind` (`up` = 1);
  - `faulttwin_monitor_cycles_total` was visible in Prometheus;
  - Grafana 12.1.0 was healthy;
  - the provisioned Prometheus datasource passed its health check;
  - the "FaultTwin self-observability" dashboard was provisioned.

  On Linux, `host.docker.internal` maps to the docker0 address, so the daemon listened on
  172.17.0.1 with `--allow-remote` for this check. Docker Desktop on macOS is expected to reach a
  loopback listener. That was **not verified**.
- No detection-quality numbers are new in Phase 5. The Phase 4 synthetic results are unchanged
  and byte-identical (§14).

## 13. Actual build and test commands
Environment:
- Ubuntu 24.04 container, Linux 6.18 x86_64, 4 vCPUs (Intel Xeon @ 2.30 GHz);
- GCC 13.3.0, Clang 18.1.3, CMake 3.28.3, Ninja 1.11.1;
- libpq 16.15, jsoncpp 1.9.5 (no OpenSSL: Trantor TLS is off);
- Docker 29.9.3, Compose v5.7.1.

```
scripts/lab-init-env.sh
docker compose up -d --wait
scripts/verify-presets.sh gcc
scripts/verify-presets.sh clang
FAULTTWIN_RESULTS_LABEL=linux-x86_64-container scripts/run-integration-tests.sh build/debug
build/debug/src/detect/faulttwin-detect evaluate --out /tmp/detect
for f in summary.json summary.md tuning-tuned.csv tuning-naive.csv tuning-knn.csv tuning-perceptron.csv alerts-test.csv alerts-test_shifted.csv; do cmp /tmp/detect/$f results/phase-04/detection/linux-x86_64-container/$f; done
find include src tests bench -name '*.cpp' -o -name '*.hpp' | xargs clang-format-18 --dry-run --Werror
cmake --preset debug -B build/tidy -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18
find src -name '*.cpp' | xargs clang-tidy-18 -p build/tidy --quiet --warnings-as-errors='*'
docker compose --profile observability up -d
```
`build/debug` is the debug preset configured with g++.

## 14. Test results
| Compiler | debug | release | asan-ubsan | tsan |
|---|---|---|---|---|
| GCC 13.3.0 | 342/342 pass | 344/344 pass | 342/342 pass | 342/342 pass |
| Clang 18.1.3 | 342/342 pass | 344/344 pass | 342/342 pass | 342/342 pass |

Release has two more tests than the other presets: `bench.smoke` and `bench.detection_smoke`.
In every preset, 23 of the 25 integration tests report `Skipped` because `FT_INTEGRATION` is
unset. The other 2 (`LibpqSessionTest.*`) need no cluster and pass.

New since Phase 4 (246 → 342 in debug):
- **74 unit tests:**
  - JSON: 10;
  - time and logger: 10;
  - metrics: 7;
  - incident model: 1;
  - memory store: 3, plus the 11 contract cases;
  - recorder: 5;
  - HTTP parsing: 2;
  - API service: 17;
  - daemon options: 7;
  - node status: 1.
- **5 CLI and end-to-end tests:** `faulttwind --help`, refusing a remote listen address,
  rejecting a short API key, rejecting an unknown option, and `e2e.faulttwind_synthetic`. The
  e2e test starts the real daemon, waits for readiness, checks about a dozen endpoints and status
  codes over HTTP, acknowledges an incident and requires exit 0 after SIGTERM.
- **17 integration tests:**
  - the 11 contract cases against PostgreSQL;
  - 5 meta-db tests: migrate is idempotent, a newer schema is refused, the one-open-incident
    index holds, the role has least privilege, 4 concurrent writers;
  - `integration.faulttwind_metadb`: the e2e script with `--store postgres`.

  Without `FT_INTEGRATION=1` these skip, and CTest reports them as `Skipped`.

**Integration run** (`FT_INTEGRATION=1`, GCC debug, lab and meta-db up): **25/25 passed**. That
is the 8 Phase 3/4 lab tests, 16 meta-db tests and the postgres e2e. Log:
`results/phase-05/integration/linux-x86_64-container.log`.

**Phase 4 determinism:** `faulttwin-detect evaluate` output matches all 8 committed files byte
for byte (`cmp`, GCC debug), after the JSON and time helpers moved to `faulttwin_core`.

**Lint:** clang-format 18 is clean. clang-tidy 18 (the repository's `.clang-tidy`, warnings as
errors) is clean over `src/`.
- Before the fixes it reported 57 findings in the new code. They were fixed by refactoring:
  - functions split below the complexity limit;
  - reference members replaced with pointers;
  - `std::ranges` algorithms;
  - raw string literals;
  - optional checks the analyser can follow.
- Four reasoned suppressions remain:
  - `misc-no-recursion` on the depth-bounded JSON parser;
  - `cppcoreguidelines-macro-usage` on the two SQL literal macros;
  - `*-avoid-c-arrays` on the atomic bucket array;
  - `misc-const-correctness` around `run()` in `main.cpp`. clang-tidy 18 flags objects that are
    mutated through references; making them const does not compile, which was checked.

**GCC release -Wnull-dereference:** the first release run failed to compile two new test files.
GCC 13 at `-O3` reports "potential null pointer dereference" on `*v.find("x")->as_string()`
written after an `EXPECT`. The tests now use small accessors that fail the test instead of
dereferencing null. Product code was not affected. The matrix in the table above was re-run
after this fix.

## 15. Sanitizer results
Every unit, CLI and e2e test ran under ASan+UBSan and under TSan, with GCC and with Clang. That
includes the real `faulttwind` daemon serving HTTP on Drogon's threads while the monitor thread
writes incidents, followed by a SIGTERM shutdown. The final run had no AddressSanitizer,
UndefinedBehaviorSanitizer or ThreadSanitizer report (`grep` over all 8 logs).

The meta-db integration tests ran under GCC debug only. They were not run under the sanitizer
presets in Phase 5.

**Found by ThreadSanitizer and fixed.** The first TSan run, with both GCC and Clang, failed
`e2e.faulttwind_synthetic` with "signal-unsafe call inside of a signal". Drogon's built-in
SIGTERM handler formats a log line (allocates memory) and then calls the user's shutdown callback
from inside the signal handler. Our callback took the logger mutex and called `app.quit()`, which
could deadlock if the signal arrived while that mutex was held. The fix is to disable Drogon's
handler, block the signals in all threads, and receive them on one thread with `sigwait`. The
matrix above was re-run after the fix.

**Found by ThreadSanitizer in Drogon, suppressed (not fixed).** The re-run after the
`TRANTOR_USE_TLS=none` change failed `e2e.faulttwind_synthetic` once under Clang TSan with a data
race between `write` and `close` on an eventfd, at shutdown. The race is inside Trantor, not in
FaultTwin code. Drogon's `quit()` calls `EventLoop::quit()` on each I/O loop from the main thread,
and `quit()` sets the stop flag *before* writing to the loop's wakeup fd. The I/O thread can see
the flag, leave its loop and run its `thread_local` `EventLoop` destructor, which closes the fd,
while the write is still in progress. Upstream Trantor's `EventLoop::quit()` has the same code.
It did not recur in 30 further Clang TSan runs or in any GCC TSan run, so it is rare.
`faulttwind` now carries a TSan suppression compiled into TSan builds only
(`__tsan_default_suppressions` in `src/server/main.cpp`), for this one pattern:
`race:trantor::EventLoop::~EventLoop`. A standalone reproduction confirmed the pattern matches the
report, which is still raised without it. Fixing it needs an upstream Trantor change.

## 16. Benchmark results
No throughput or latency benchmark was run for the API in Phase 5. API latency and load are
Phase 11 work (Experiment 5), and no API performance number is claimed.

**Build cost (ADR-0021 / ADR-0004 decision input)**, one run, release preset, fresh build
directory, Drogon source already downloaded, 4 vCPUs, GCC 13.3
(`results/phase-05/build-cost/linux-x86_64-container.txt`):

| Step | Wall time |
|---|---|
| Configure (includes Drogon's CMake) | 8.0 s |
| Build Drogon + Trantor (`--target drogon`, -j4) | 50.4 s |
| Build everything else (all FaultTwin targets, tests, benchmarks, -j4) | 66.2 s |

`libdrogon.a` plus `libtrantor.a` take 10.8 MB, and the release `faulttwind` binary is 3.6 MB.
Each CI matrix job builds Drogon once, so expect about 50 s more per job on a 4-vCPU runner
(not yet observed in CI).

## 17. Baseline comparison
There is no performance baseline in this phase.

The functional baseline is the in-memory store:
- the PostgreSQL store passes exactly the same 11 contract cases;
- the API behaves the same with either store, because the e2e script passes with both.

## 18. Raw evidence artifact locations
- `results/phase-05/{gcc,clang}/`: environment and full log for each preset.
- `results/phase-05/integration/linux-x86_64-container.log`: the `FT_INTEGRATION=1` run (25/25).
- `results/phase-05/observability/linux-x86_64-container.txt`: Prometheus and Grafana checks.
- `results/phase-05/build-cost/linux-x86_64-container.txt`: Drogon build timing.
- The `git_commit` in the environment files is the container's throw-away tracking repository,
  not the owner's repository (see §20).

## 19. Updated evidence ledger
- `docs/resume/evidence-ledger.md` gains a Phase 5 functional table. Every row is TESTED
  (container) or NOT RUN. No resume metric is added.
- Also updated:
  - `docs/evaluation/verified-results.md`;
  - `docs/evaluation/limitations.md`;
  - `docs/architecture/acceptance-criteria.md` (criteria 5.1–5.14);
  - `docs/architecture/dependencies.md`;
  - `docs/PROGRESS.md`.

## 20. Git status and commit SHA
- **Base and repository state.**
  - The base is `6209dee` on `main`. The assistant ran **no** git command in the owner's
    repository.
  - The container tracked its own edits in a throw-away repository with a scratch identity. That
    repository is not the owner's.
  - All Phase 5 files are uncommitted changes in the owner's working tree.
- **CI file.** The file tool cannot write into `.github/`, so the CI change is `ci.phase-05.yml`
  at the repository root. Its changes:
  - installs `libjsoncpp-dev uuid-dev zlib1g-dev curl` in every build job;
  - log and artifact paths move to `phase-05`;
  - the integration job now also brings up the meta-db (no profile needed). Its
    `FT_INTEGRATION=1` run includes the meta-db tests and the postgres e2e.

  The byte-for-byte Phase 4 reproduction step is kept unchanged.
- **Authorship.** No file names Claude, Anthropic or any AI as author or co-author.

Suggested commit message (for the owner to review and commit):
```
Phase 5: incident storage, versioned HTTP API, metrics and logging

- core: strict bounded JSON parser/writer, ISO-8601 and epoch-us time,
  JSON-lines logger with secret redaction
- observability: Prometheus registry (counters, gauges, histograms)
  with per-family series cap and overflow counter
- storage: incident model, IncidentStore with memory and PostgreSQL
  implementations (separate meta-db, single-statement writes, bind
  parameters, versioned migrations), recorder with ordered retry queue
- api: /api/v1 incidents, nodes, version; healthz, readyz, metrics;
  Bearer auth for acknowledge; one error format; OpenAPI contract
- faulttwind: monitor loop + recorder + Drogon v1.9.9 (pinned
  FetchContent), loopback by default, signals taken on a dedicated
  thread, clean SIGTERM drain
- compose: meta-db, opt-in Prometheus and Grafana with dashboard
- libpq session: parameterised queries; read-only stays the default
- tests (unit, contract, e2e, meta-db integration), ADR-0019..0022,
  docs, evidence and CI updates
```

## 21. Known bugs and limitations
No known bugs. Limitations, also in `docs/evaluation/limitations.md`:
1. **Read endpoints have no authentication**, and there is no TLS or rate limiting. Loopback is
   the default, and `--allow-remote` is explicit. Phase 11.
2. **One meta-db connection behind a mutex.** API reads and recorder writes queue behind each
   other. This is fine at lab rates and was not measured under load.
3. **No API performance measurement** (§16).
4. The retry queue is in memory. If the daemon is killed during a meta-db outage, the queued
   detections are lost (the count is in the logs and metrics). The next start closes orphans as
   `monitor_restarted`.
5. Two processes migrating at the same moment are protected by an advisory lock, but that race is
   not tested.
6. `/api/v1/nodes` shows only the last cycle. There is no history endpoint. History comes in
   Phase 10.
7. The synthetic source replays Phase 4 scenarios, so its values are generated. The daemon logs a
   warning when it starts with it.
8. A Prometheus scrape of a loopback listener was verified only on Linux, via docker0 and
   `--allow-remote`. On Docker Desktop for macOS it is expected but unverified.
9. All measurements ran in one 4-vCPU cloud container. The Mac run checked correctness only. CI has
   not run.
10. The first macOS build failed inside Trantor (Drogon's network library): its OpenSSL provider
    does not compile against Homebrew OpenSSL 4.1.0. Trantor is now built with
    `TRANTOR_USE_TLS=none`, so `faulttwind` has no TLS and no OpenSSL dependency. It serves plain
    HTTP on loopback; TLS belongs to Phase 11 hardening.

## 22. Incomplete requirements
- **Still to do by the owner:**
  - moving `ci.phase-05.yml` into place and a green CI run;
  - review and approval.
- **NOT RUN:** macOS release and sanitizer presets; CI with the Phase 5 workflow; API load and latency
  measurement; the macOS Prometheus/Grafana check.
- **Deferred with an ADR:** gRPC telemetry transport (ADR-0022).
- **Not implemented:** TLS, authentication for reads, rate limiting (Phase 11); incident
  history and trend endpoints (Phase 10).

## 23. Questions requiring architecture review
1. **Reads without auth.** Should Phase 5 add an optional read key (`FT_API_READ_KEY`), or is
   loopback plus `--allow-remote` acceptable until Phase 11? I recommend waiting. The dashboard
   (Phase 10) will decide what the read authentication model needs.
2. **Orphan policy.** At startup, open incidents are closed as `monitor_restarted`. The
   alternative is to resume them if the same alert is still firing. Closing is simpler and
   honest about the gap. Is that acceptable?
3. **Retry queue durability.** Is a bounded in-memory queue acceptable, or should detections
   during a meta-db outage go to a local append-only file? A file would be the first real use of
   the Protobuf record format from ADR-0003.
4. **CI time.** Drogon adds about 50 s per matrix job (measured locally, §16). Is that
   acceptable on every push, or should Drogon be cached (`actions/cache` on `build/_deps`)?
5. **Grafana licence.** Grafana is AGPL-3.0. It runs only as a separate container and is not
   linked or modified. Is that acceptable for the portfolio's licence choice?

## 24. Practical learning exercises
1. Stop the meta-db (`docker compose stop meta-db`) with `faulttwind --store postgres` running.
   Watch `/readyz`, `faulttwin_incident_recorder_pending` and the logs. Start it again. Are the
   queued incidents written in order?
2. Remove the unique partial index from the migration and run the contract suite against
   PostgreSQL. Which test catches it, and why can the memory store not catch the same bug?
3. Replace keyset pagination with `OFFSET` and write a test that inserts between two page reads.
   What goes wrong?
4. Add a `path` label (the raw path) to `faulttwin_http_requests_total` and hit `/api/v1/incidents/1`
   … `/9999`. What happens to `/metrics`, and what does the overflow counter show?
5. Send `{"by":"a","by":"b"}` to acknowledge. Why does the parser reject duplicate keys?
6. Run `faulttwind --listen 0.0.0.0:8080`. Why does it refuse, and what does `--allow-remote`
   tell an operator?

## 25. Interview questions with explanations
1. **Why keep incidents outside the monitored database?** A monitor that writes its evidence
   into the system that is failing loses the evidence exactly when it matters. Fault injection
   would also corrupt the observer. A separate store on a separate network removes both risks.
2. **How do you make a multi-step database change atomic without a transaction?** Use a single
   statement with data-modifying CTEs. PostgreSQL runs the whole statement atomically, and one
   round trip also cuts latency.
3. **Why liveness and readiness as separate endpoints?** Liveness means "restart me if this
   fails". Readiness means "do not send me traffic". A daemon whose store is down should not be
   restarted in a loop. It should report not-ready until the store returns.
4. **What is label cardinality, and why cap it?** Each distinct label combination is a separate
   time series held in memory by both the exporter and Prometheus. An unbounded label such as a
   raw path or a user id can exhaust memory, so the registry caps each family and counts the
   overflow.
5. **Why keep HTTP framework types out of the API layer?** The API logic can then be
   unit-tested as a pure function, without sockets or a running server, and the framework can be
   replaced without touching business rules.
6. **How do you compare an API key safely?** Use a constant-time comparison over the whole key,
   so that response time does not reveal how many leading characters matched. Also refuse to run
   with a short key, and default to no writes rather than open writes.
7. **Why keyset pagination?** It is O(limit) per page with an index, and it is stable when new
   rows arrive. Offset pagination skips or repeats rows under concurrent inserts, and gets slower
   for deep pages.

## 26. Phase acceptance criteria and current status
From `docs/architecture/acceptance-criteria.md` (Phase 5 criteria made explicit this phase):

| # | Criterion | Status |
|---|---|---|
| 5.1 | Incident model and lifecycle (open, escalate, resolve, orphans, acknowledgement) | ✅ tested |
| 5.2 | Persistent incident store outside the monitored cluster, least-privilege role, versioned migrations | ✅ tested (integration, Linux container) |
| 5.3 | Same contract for the memory and PostgreSQL stores | ✅ tested (11 cases each) |
| 5.4 | Store outages do not lose or reorder accepted detections (bounded, counted) | ✅ tested |
| 5.5 | Versioned REST API under `/api/v1` with an OpenAPI contract, strict validation and one error format | ✅ tested |
| 5.6 | Drogon HTTP service; clean shutdown on SIGTERM | ✅ tested (e2e) |
| 5.7 | Health and readiness endpoints that reflect store and monitor state | ✅ tested |
| 5.8 | Prometheus metrics with bounded cardinality; Prometheus and Grafana provisioned | ✅ tested; scrape verified (Linux container) |
| 5.9 | Structured logs with secret redaction | ✅ tested |
| 5.10 | gRPC only with a recorded justification | ✅ deferred, ADR-0022 |
| 5.11 | All presets pass with GCC and Clang; tidy and format clean; Phase 4 output unchanged | ✅ tested (Linux container) |
| 5.12 | Builds and tests on the owner's Mac | ✅ owner-run (Apple Silicon, AppleClang, Homebrew OpenSSL 4.1 present but unused): debug 342/342 including the `faulttwind` CLI and end-to-end tests, integration 25/25 with the lab and the meta-db, after the `TRANTOR_USE_TLS=none` build fix |
| 5.13 | CI green with the Phase 5 workflow | ⏳ NOT RUN |
| 5.14 | No AI authorship; no assistant commits; owner approval after review | ⏳ approval |

**Phase 5 is not complete.** Still open: CI (5.13) and your review and
approval (5.14). Phase 6 has not been started.
