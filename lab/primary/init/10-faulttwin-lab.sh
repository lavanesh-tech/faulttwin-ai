#!/usr/bin/env bash
# Runs once, when the primary's data directory is first initialised (docker-entrypoint-initdb.d).
# Creates the lab roles, the workload schema, pg_stat_statements and one physical replication
# slot per replica. Passwords come from the environment (.env); nothing secret is in this file.
set -euo pipefail

: "${FT_REPLICATION_PASSWORD:?}" "${FT_MONITOR_PASSWORD:?}" "${FT_WORKLOAD_PASSWORD:?}"

psql -v ON_ERROR_STOP=1 --username "${POSTGRES_USER}" --dbname "${POSTGRES_DB}" \
     -v repl_pw="${FT_REPLICATION_PASSWORD}" \
     -v mon_pw="${FT_MONITOR_PASSWORD}" \
     -v work_pw="${FT_WORKLOAD_PASSWORD}" <<'SQL'
-- Replication: may only stream WAL, cannot log in to a database.
CREATE ROLE ft_replicator WITH LOGIN REPLICATION PASSWORD :'repl_pw';

-- Monitoring: least privilege. pg_monitor grants read access to statistics views and
-- monitoring functions only. Every transaction is read-only by default, statements are
-- time-limited, and the role cannot hold more than 5 connections.
CREATE ROLE ft_monitor WITH LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION NOBYPASSRLS
    CONNECTION LIMIT 5 PASSWORD :'mon_pw';
GRANT pg_monitor TO ft_monitor;
ALTER ROLE ft_monitor SET default_transaction_read_only = on;
ALTER ROLE ft_monitor SET statement_timeout = '5s';
ALTER ROLE ft_monitor SET lock_timeout = '1s';
ALTER ROLE ft_monitor SET idle_in_transaction_session_timeout = '10s';

-- Nobody but owners may create objects or temporary tables in the lab database.
REVOKE CREATE ON SCHEMA public FROM PUBLIC;
REVOKE TEMPORARY ON DATABASE postgres FROM PUBLIC;

-- Workload: owns the application schema used by integration tests and load generation.
CREATE ROLE ft_workload WITH LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE PASSWORD :'work_pw';
CREATE SCHEMA lab_app AUTHORIZATION ft_workload;
CREATE TABLE lab_app.orders (
    id          bigserial PRIMARY KEY,
    created_at  timestamptz NOT NULL DEFAULT now(),
    amount      numeric(12, 2) NOT NULL
);
ALTER TABLE lab_app.orders OWNER TO ft_workload;
-- The monitoring role must not be able to read application data.
REVOKE ALL ON SCHEMA lab_app FROM PUBLIC;

CREATE EXTENSION IF NOT EXISTS pg_stat_statements;

SELECT pg_create_physical_replication_slot('pg_replica_1');
SELECT pg_create_physical_replication_slot('pg_replica_2');
SQL

# Replicas connect from the lab network only (samenet = the container's own subnet).
cat >> "${PGDATA}/pg_hba.conf" <<'HBA'
# FaultTwin lab: streaming replication from the Compose network only
host    replication     ft_replicator   samenet                 scram-sha-256
HBA
