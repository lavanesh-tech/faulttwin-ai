#!/usr/bin/env bash
# Create .env from .env.example with random passwords. Never overwrites an existing .env.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
env_file="${root}/.env"
if [[ -e "${env_file}" ]]; then
  # Phase 5 added keys. Append any that an older .env lacks, with fresh random values;
  # existing lines are never changed.
  added=0
  while IFS= read -r line; do
    key="${line%%=*}"
    [[ -z "${line}" || "${line}" == \#* || "${line}" != *=* ]] && continue
    if ! grep -q "^${key}=" "${env_file}"; then
      value="${line#*=}"
      case "${key}" in
        *PASSWORD|FT_API_KEY) value="$(openssl rand -hex 16)" ;;
      esac
      printf '%s=%s\n' "${key}" "${value}" >> "${env_file}"
      added=$((added + 1))
    fi
  done < "${root}/.env.example"
  echo ".env already exists; added ${added} missing key(s), changed nothing else"
  exit 0
fi
umask 077
rand() { openssl rand -hex 16; }
sed -e "s/^POSTGRES_PASSWORD=.*/POSTGRES_PASSWORD=$(rand)/" \
    -e "s/^FT_REPLICATION_PASSWORD=.*/FT_REPLICATION_PASSWORD=$(rand)/" \
    -e "s/^FT_MONITOR_PASSWORD=.*/FT_MONITOR_PASSWORD=$(rand)/" \
    -e "s/^FT_WORKLOAD_PASSWORD=.*/FT_WORKLOAD_PASSWORD=$(rand)/" \
    -e "s/^FT_METADB_PASSWORD=.*/FT_METADB_PASSWORD=$(rand)/" \
    -e "s/^FT_API_KEY=.*/FT_API_KEY=$(rand)/" \
    -e "s/^FT_GRAFANA_ADMIN_PASSWORD=.*/FT_GRAFANA_ADMIN_PASSWORD=$(rand)/" \
    "${root}/.env.example" > "${env_file}"
echo "wrote ${env_file} (mode 600)"
