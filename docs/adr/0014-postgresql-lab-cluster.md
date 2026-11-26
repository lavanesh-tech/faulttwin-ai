# ADR-0014: PostgreSQL lab cluster — 17.12 pinned by digest, async streaming with physical slots

- Status: Accepted (Phase 3)
- Date: 2026-11-26

## Context
Phase 3 needs a real primary and two streaming replicas for collectors and integration tests,
on Linux (CI) and on the owner's Apple Silicon Mac (Docker Desktop). `dependencies.md` left the
choice between PostgreSQL 17.x and 18.x open until this phase.

## Decision
- **Version:** PostgreSQL 17.12, image `postgres:17.12-bookworm` pinned to its multi-arch index
  digest `sha256:1eff563f…50d7`, so amd64 and arm64 resolve the same release and a re-pushed tag
  cannot change the lab silently. 17 is the mature major with the monitoring views used here
  (`pg_stat_replication`, `pg_stat_wal_receiver`, `pg_stat_statements`, `pg_stat_force_next_flush`);
  nothing in Phase 3 needs 18.
- **Topology:** one primary, two asynchronous physical streaming replicas cloned with
  `pg_basebackup` on first start, each with a pre-created physical replication slot
  (`pg_replica_1`, `pg_replica_2`). `max_slot_wal_keep_size=1024MB` bounds the WAL a dead replica
  can pin. Every node runs with its own `cluster_name`, which a replica reports to the primary as
  `application_name`; the collector matches replication edges by it.
- **Roles (created once by `lab/primary/init/10-faulttwin-lab.sh`):**
  - `ft_replicator`: `LOGIN REPLICATION`, allowed only for `replication` connections from the
    Compose network (`samenet`) in `pg_hba.conf`.
  - `ft_monitor`: member of `pg_monitor` only; `default_transaction_read_only=on`,
    `statement_timeout=5s`, `lock_timeout=1s`, `CONNECTION LIMIT 5`; no `CREATE` on `public`, no
    `TEMPORARY` on the database, no access to the application schema.
  - `ft_workload`: owns schema `lab_app`, used by tests and pgbench.
- **Secrets:** passwords live only in `.env` (git-ignored; `.env.example` is committed;
  `scripts/lab-init-env.sh` fills random values). The replication password reaches the replicas
  through a `0600` passfile, never `argv` or `postgresql.auto.conf` (the entrypoint refuses to start
  if a password appears there).
- **Isolation:** one bridge network `faulttwin-lab`; host ports bound to `127.0.0.1` only. All
  containers carry `faulttwin.role=lab-target` and every server runs with `faulttwin.lab=on`, the
  markers the Phase 6 fault-injection guards will require (overview §6.9).

## Alternatives considered
- PostgreSQL 18: newest features, but less field time; no Phase 3 need.
- Patroni / repmgr: real HA managers, but they would make failover automatic and hide exactly the
  behaviour FaultTwin is meant to observe and, later, recommend actions for.
- Replicas without slots: no WAL retention guarantee; slots also give the collector a "retained
  WAL" signal. The cost (WAL pinned by a dead replica) is bounded by `max_slot_wal_keep_size`.
- A Compose `internal: true` network: would block publishing ports to the host collector.

## Consequences
- `docker compose up -d --wait` brought up a healthy cluster from empty volumes in 11.3 s (3 runs: 11.36, 11.29, 11.27 s, image already pulled) in the Linux container
  used for Phase 3 (first pull excluded).
- The image's default `pg_hba.conf` trusts connections from inside each container; the healthcheck
  and the measurement script's superuser commands rely on it. Host connections need passwords.
- No TLS in the lab (`sslmode=prefer` falls back to plaintext on loopback/bridge). Recorded in
  limitations.

## Revisit when
TLS, synchronous replication or cascading replicas are needed, or 18.x becomes the default target.
