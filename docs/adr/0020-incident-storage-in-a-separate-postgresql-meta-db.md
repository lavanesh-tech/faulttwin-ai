# ADR-0020: Incident storage in a separate PostgreSQL meta-db

- Status: Accepted (Phase 5)
- Date: 2026-11-28

## Context
Phase 5 must persist incidents: what fired, on which node, how it escalated, when it ended,
and who acknowledged it. Phase 4 detections are transient. Several constraints apply:
- Later phases inject faults into the monitored PostgreSQL cluster (Phase 6). Records about
  an outage must not live on the system that is having the outage.
- The store must keep working when the monitored cluster is down.
- Writes come from the monitor loop and reads come from the HTTP threads, at the same time.
- ADR-0003 proposed Protobuf for an "incident log" file format.

## Decision
1. **Incident model.** One incident is the lifetime of one alert for a (rule, node) pair,
   from `fired` to `resolved`. It holds at most 3 events: fired, escalated and resolved.
   - The resolution is `recovered` or `monitor_restarted`. The second is used when
     `faulttwind` starts and closes incidents that a previous run left open (orphans).
   - An acknowledgement (`by`, `note`, `at`) is written once and never overwritten.
   - All limits (identifier lengths, note size, evidence items) are checked in C++, and the
     database repeats them as `CHECK` constraints.
2. **One interface, two implementations.** `IncidentStore` is implemented by:
   - `MemoryIncidentStore`: bounded. It evicts the oldest resolved incident when full, and
     refuses rather than drops when every incident is still open.
   - `PgIncidentStore`: the default for real runs.

   The same contract test suite (11 cases) runs against both. The PostgreSQL run is in the
   integration suite.
3. **Separate meta-db.** The database is its own `postgres:17.12` service (`meta-db`). It has:
   - its own Docker network, `meta`, shared with no monitored node;
   - its own volume;
   - its own least-privilege role `ft_app`: no superuser, `CREATEDB` or `CREATEROLE`;
     connection limit 10; it owns only schema `faulttwin`; PUBLIC privileges are revoked.

   Collectors still connect read-only to the monitored cluster. Only the meta-db session
   turns `read_only` off.
4. **SQL discipline in `PgIncidentStore`:**
   - Every operation is a single statement. Writes use data-modifying CTEs, so each one is
     atomic without an explicit transaction, and each read sees one snapshot.
   - Every value is a bind parameter (`PQsendQueryParams`). No SQL is built from input.
   - Times are stored as `timestamptz` and travel as integer epoch microseconds in both
     directions, with no floating point.
   - Evidence is stored as `json`, not `jsonb`, so key order is kept and a stored incident
     reads back byte for byte.
   - A unique partial index enforces at most one open incident per (rule_id, node). A
     violation maps to `kConflict`.
5. **Migrations.**
   - `migrate()` runs inside one transaction under an advisory lock, so two processes
     starting together cannot race.
   - It records versions in `faulttwin.schema_migrations`.
   - It refuses to run against a schema newer than the build (`kInvalid`, and startup exits
     with code 3).
6. **Failure behaviour.**
   - A connection failure or client deadline drops the session and returns `kUnavailable`.
     The next call reconnects.
   - `IncidentRecorder` keeps detections it could not write in a bounded, ordered retry
     queue (default 1024). It retries them in order on the next cycle. On overflow it drops
     and counts.
   - `/readyz` reports the store as down. `GET` endpoints answer 503 with `Retry-After`.

## Alternatives considered
- **Store incidents in the monitored cluster:** violates the isolation needed for Phase 6
  fault injection.
- **SQLite file:** no network dependency, but it is a second SQL dialect and it does not
  serve several `faulttwind` instances later (Phase 11). We already depend on libpq.
- **Protobuf append-only log (ADR-0003):** good for replay, but it needs its own indexing to
  answer "open incidents on node X", plus compaction and acknowledgement updates. It is
  deferred. Phase 7 and Phase 9 can export incidents to a file format if replay needs one.
- **ORM (Drogon ORM):** hides the SQL that this ADR relies on being explicit. It is disabled
  in the build.

## Consequences
- The lab grows by one container (40 MiB RAM measured by `docker stats` after the integration run, Linux container).
- One connection behind a mutex serialises API reads and recorder writes. That is enough at
  lab request rates, and it is revisited in Phase 11 (pool or read replica).
- Incidents survive `faulttwind` restarts. Open ones are closed as `monitor_restarted`
  rather than silently continued, because the monitor did not observe the gap.

## Revisit when
Several `faulttwind` instances run (Phase 11), or incident volume needs retention or
partitioning.
