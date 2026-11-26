#!/usr/bin/env bash
# Create .env from .env.example with random passwords. Never overwrites an existing .env.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
env_file="${root}/.env"
if [[ -e "${env_file}" ]]; then
  echo ".env already exists; leaving it unchanged"
  exit 0
fi
umask 077
rand() { openssl rand -hex 16; }
sed -e "s/^POSTGRES_PASSWORD=.*/POSTGRES_PASSWORD=$(rand)/" \
    -e "s/^FT_REPLICATION_PASSWORD=.*/FT_REPLICATION_PASSWORD=$(rand)/" \
    -e "s/^FT_MONITOR_PASSWORD=.*/FT_MONITOR_PASSWORD=$(rand)/" \
    -e "s/^FT_WORKLOAD_PASSWORD=.*/FT_WORKLOAD_PASSWORD=$(rand)/" \
    "${root}/.env.example" > "${env_file}"
echo "wrote ${env_file} (mode 600)"
