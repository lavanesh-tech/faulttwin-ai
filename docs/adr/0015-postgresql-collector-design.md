# ADR-0015: PostgreSQL collector — libpq C API, one read-only transaction per cycle, explicit reading quality

- Status: Accepted (Phase 3)
- Date: 2026-11-26

## Context
FR-MON-1 asks for availability, connections, transaction throughput, query latency, WAL,
replication lag/state, replay positions and role/topology from every node, with read-only
credentials, and the specification forbids confusing missing, stale or invalid telemetry with
healthy values (ADR-0010 established "missing ≠ zero" for the Phase 1 model).

## Decision
1. **Client: libpq's C API, wrapped in RAII** (`LibpqSession`, `unique_ptr` deleters for
   `PGconn`/`PGresult`). libpq is the reference client, available on every platform (`libpq-dev`,
   Homebrew `libpq`), and needed anyway. All parsing sits behind a three-line `SqlSession`
   interface, so the collection logic is unit-tested with a scripted fake and only the adapter needs a
   server. The adapter and `faulttwin-monitor` are built only when CMake finds libpq
   (`FAULTTWIN_WITH_LIBPQ=AUTO|ON|OFF`); everything else builds without it.
2. **Timeouts on both sides.** Server: `statement_timeout`, `lock_timeout` and
   `default_transaction_read_only=on` in the connection options, on top of the role defaults.
   Client: `connect_timeout`, and every query is sent asynchronously and polled against a deadline.
   On expiry the connection is closed and the node reported down (`timeout`). A frozen server
   (`docker pause`) therefore costs one deadline, not a hung collector (integration-tested).
3. **One read-only REPEATABLE READ transaction per collection.** The statistics views are then
   read from one snapshot, and the collector adds exactly one commit per cycle to the node's
   counters. It calls `pg_stat_force_next_flush()` (PostgreSQL ≥ 15) before committing, so that
   commit is visible by the next cycle, and `compute_rates` subtracts it. Measured before this
   change: ~16 phantom commits/s per node from the collector's own autocommit statements. A failed
   statement rolls back and starts a new transaction, so one denied view cannot blank the cycle.
4. **Every value is a `Reading<T>` with a `Quality`:** `ok`, `missing`, `stale`, `invalid`,
   `not_applicable`, plus a reason. `value()` is engaged only for `ok` and `stale`. NULL from the
   server is `missing` (never 0); unparsable, negative or out-of-order values are `invalid`;
   metrics that do not apply to the role (replica lag on a primary) are `not_applicable`; a
   snapshot older than `max_age` at consumption is `stale` with the value kept for context.
5. **Lag semantics, chosen from what the lab showed:**
   - Byte lag from the primary's view (`pg_current_wal_lsn() - replay_lsn` in the same statement).
   - `pg_stat_replication.*_lag` is reported as-is; PostgreSQL resets it to NULL when the standby
     is caught up and idle, so it is `missing` with that reason.
   - Replica-side time lag is **not** `now() - pg_last_xact_replay_timestamp()`: on an idle
     primary that reported 474 s for a fully caught-up replica. The replica reports 0 s only when
     its receiver is streaming and everything received is replayed; otherwise `missing`. The
     timestamp age is exported separately as `last_replay_age_seconds`.
   - After a WAL receiver restarts, its receive LSN can be *behind* the replay LSN (it reports the
     segment-aligned streaming start). That is treated as "nothing pending", not as invalid data.
   - A receiver that has not heard from the primary for longer than `max_receiver_silence`
     (default 30 s) makes the receive-derived readings `stale`.
6. **Topology is derived, not configured:** roles from `pg_is_in_recovery()`, edges from the
   primary's `pg_stat_replication` matched to inventory names via `cluster_name`, with explicit
   issues: `node_down`, `role_unknown`, `no_reachable_primary`, `multiple_primaries`,
   `replica_not_streaming`, `replica_not_seen_by_primary`, `unknown_standby`, `slot_inactive`.
7. **Host resources** come from procfs (`/proc/stat`, `/proc/meminfo`, `/proc/loadavg`) with an
   injectable root, plus `std::filesystem::space`. Where procfs does not exist (macOS) the readings
   are `missing` with that reason.

## Alternatives considered
- libpqxx: nicer C++ API, but another dependency, and it throws exceptions for the error paths
  that are normal here (timeouts, denied views).
- One autocommit statement per metric: simplest, but inconsistent snapshots and a measurable
  observer effect on transaction throughput (point 3).
- `PQcancel` on timeout: needs a new TCP connection to a server that is, by assumption, not
  answering; closing the connection is bounded and simpler.
- Running collectors inside each database container: would give per-container resources, but
  needs an agent and privileges the core must not have (overview §3). Deferred.

## Consequences
- The Phase 1 `TelemetrySample` has non-optional CPU/memory/disk fields, so it cannot carry a
  `missing` resource reading. Phase 3 collectors therefore produce their own snapshot types and do
  not feed the Phase 2 pipeline yet (architecture question in the Phase 3 report).
- Connection-failure classification relies on libpq's English message text (it exposes no
  SQLSTATE for connection errors).
- Collection is sequential across nodes: a down node costs up to `connect_timeout` per cycle.

## Revisit when
Per-node parallel collection is needed (large clusters), the telemetry model is extended for
optional resources, or a remote agent is introduced (ADR-0003).
