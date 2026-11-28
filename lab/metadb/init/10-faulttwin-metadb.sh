#!/usr/bin/env bash
# Runs once, when the meta-db data directory is first initialised. Creates FaultTwin's own
# database and the application role. faulttwind creates and upgrades its tables itself
# (PgIncidentStore::migrate) inside the faulttwin schema, which this role owns and which is
# the only place it can create objects. The password comes from .env.
set -euo pipefail

: "${FT_METADB_PASSWORD:?}"

psql -v ON_ERROR_STOP=1 --username "${POSTGRES_USER}" --dbname "${POSTGRES_DB}" \
     -v app_pw="${FT_METADB_PASSWORD}" <<'SQL'
CREATE ROLE ft_app WITH LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION NOBYPASSRLS
    CONNECTION LIMIT 10 PASSWORD :'app_pw';
CREATE DATABASE faulttwin OWNER postgres;
REVOKE ALL ON DATABASE faulttwin FROM PUBLIC;
GRANT CONNECT ON DATABASE faulttwin TO ft_app;
ALTER ROLE ft_app SET statement_timeout = '5s';
ALTER ROLE ft_app SET lock_timeout = '2s';
ALTER ROLE ft_app SET idle_in_transaction_session_timeout = '30s';
SQL

psql -v ON_ERROR_STOP=1 --username "${POSTGRES_USER}" --dbname faulttwin <<'SQL'
REVOKE CREATE ON SCHEMA public FROM PUBLIC;
REVOKE TEMPORARY ON DATABASE faulttwin FROM PUBLIC;
CREATE SCHEMA faulttwin AUTHORIZATION ft_app;
ALTER ROLE ft_app IN DATABASE faulttwin SET search_path = faulttwin;
SQL
