#!/usr/bin/env bash
# Run the integration tests against the Docker Compose lab cluster.
#
# Usage: scripts/run-integration-tests.sh [build-dir]     (default: build/debug)
#
# Loads .env, checks that Docker and the cluster are up (starting the cluster if needed),
# then runs `ctest -L integration` with FT_INTEGRATION=1 and saves the log under
# results/${FAULTTWIN_RESULTS_PHASE:-phase-04}/integration/.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-${root}/build/debug}"
phase="${FAULTTWIN_RESULTS_PHASE:-phase-04}"
out="${root}/results/${phase}/integration"
label="${FAULTTWIN_RESULTS_LABEL:-$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)}"

if [[ ! -f "${root}/.env" ]]; then
  echo "error: ${root}/.env not found; run scripts/lab-init-env.sh first" >&2
  exit 2
fi
if ! docker info >/dev/null 2>&1; then
  echo "error: Docker is not running" >&2
  exit 2
fi
if [[ ! -x "${build_dir}/tests/faulttwin_integration_tests" ]]; then
  echo "error: ${build_dir}/tests/faulttwin_integration_tests not built (is libpq installed?)" >&2
  exit 2
fi

set -a
# shellcheck disable=SC1091
. "${root}/.env"
set +a

# Docker reports a container as unhealthy while it is paused and until the next healthcheck
# after it is unpaused (about 2 s), and `up --wait` fails at once on unhealthy. The paused-replica
# test leaves the lab in that state for a moment, so retry before giving up.
for attempt in 1 2 3 4 5; do
  if (cd "${root}" && docker compose up -d --wait); then
    break
  fi
  if [[ ${attempt} -eq 5 ]]; then
    echo "error: the lab cluster did not become healthy" >&2
    exit 1
  fi
  sleep 3
done

mkdir -p "${out}"
log="${out}/${label}.log"
{
  echo "date_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "uname: $(uname -srm)"
  echo "docker: $(docker version --format '{{.Server.Version}}' 2>/dev/null || echo unknown)"
  echo "compose: $(docker compose version --short 2>/dev/null || echo unknown)"
  echo "postgres: $(cd "${root}" && docker compose exec -T pg-primary postgres --version)"
  echo "git_commit: $(git -C "${root}" rev-parse HEAD 2>/dev/null || echo 'not a git repository')"
  echo
} > "${log}"

status=0
FT_INTEGRATION=1 ctest --test-dir "${build_dir}" -L integration --output-on-failure -V \
  >> "${log}" 2>&1 || status=$?
grep -E '\[ measured \]|tests passed|tests failed' "${log}" || true
echo "log: ${log}"
exit "${status}"
