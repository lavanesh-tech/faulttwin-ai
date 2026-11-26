#!/usr/bin/env bash
# Replica bootstrap. On first start (empty data directory) clone the primary with
# pg_basebackup using this replica's pre-created physical slot, then hand over to the
# image's normal entrypoint, which sees a populated data directory and skips initdb.
set -euo pipefail

: "${FT_PRIMARY_HOST:?}" "${FT_REPLICATION_SLOT:?}" "${FT_REPLICATION_PASSWORD:?}"
PGDATA="${PGDATA:-/var/lib/postgresql/data}"

if [[ ! -s "${PGDATA}/PG_VERSION" ]]; then
  echo "replica-entrypoint: cloning ${FT_PRIMARY_HOST} with slot ${FT_REPLICATION_SLOT}"
  mkdir -p "${PGDATA}"
  chown postgres:postgres "${PGDATA}"
  chmod 700 "${PGDATA}"

  # The password is never put on a command line or into postgresql.auto.conf. The clone reads
  # it from a temporary 0600 passfile (pg_basebackup -R would copy a PGPASSWORD value into
  # primary_conninfo; with a passfile it records only the file's path). Afterwards the
  # passfile moves into the data volume and primary_conninfo points at it, so the WAL
  # receiver can reconnect after restarts.
  tmp_passfile="$(mktemp)"
  printf '%s:5432:replication:ft_replicator:%s\n' "${FT_PRIMARY_HOST}" "${FT_REPLICATION_PASSWORD}" > "${tmp_passfile}"
  chown postgres:postgres "${tmp_passfile}"
  chmod 600 "${tmp_passfile}"
  for attempt in $(seq 1 30); do
    if gosu postgres env PGPASSFILE="${tmp_passfile}" pg_basebackup \
        --host="${FT_PRIMARY_HOST}" --port=5432 --username=ft_replicator \
        --pgdata="${PGDATA}" --wal-method=stream --slot="${FT_REPLICATION_SLOT}" \
        --write-recovery-conf --checkpoint=fast --no-password; then
      break
    fi
    if [[ "${attempt}" -eq 30 ]]; then
      echo "replica-entrypoint: pg_basebackup failed 30 times; giving up" >&2
      exit 1
    fi
    echo "replica-entrypoint: pg_basebackup attempt ${attempt} failed; retrying in 2s"
    find "${PGDATA}" -mindepth 1 -delete
    sleep 2
  done

  passfile="${PGDATA}/.ft_replication_pgpass"
  mv "${tmp_passfile}" "${passfile}"
  # pg_basebackup -R recorded the temporary passfile path; point it at the permanent one.
  sed -i "s|passfile=''${tmp_passfile}''|passfile=''${passfile}''|" "${PGDATA}/postgresql.auto.conf"
  if ! grep -q "passfile=''${passfile}''" "${PGDATA}/postgresql.auto.conf"; then
    echo "replica-entrypoint: could not set passfile in primary_conninfo" >&2
    exit 1
  fi
  if grep -q 'password=' "${PGDATA}/postgresql.auto.conf"; then
    echo "replica-entrypoint: refusing to start, a password was written to postgresql.auto.conf" >&2
    exit 1
  fi
fi

exec docker-entrypoint.sh "$@"
