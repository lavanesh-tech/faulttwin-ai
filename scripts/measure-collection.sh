#!/usr/bin/env bash
# Phase 3 collection measurements (MON-SUCC, MON-COV, MON-OVH, MON-FRESH) on the lab cluster.
#
# Usage: scripts/measure-collection.sh [build-dir] [repetitions]
#        defaults: build/release, 3
#
# A. Idle cluster: 300 cycles at 200 ms. Client wall/CPU per cycle, freshness, coverage and
#    success from faulttwin-monitor; server execution time of the monitor's own statements
#    from pg_stat_statements on every node.
# B. Throughput impact: pgbench (4 clients, 15 s, one insert per transaction) without the
#    monitor, with the monitor at 1000 ms and at 100 ms, interleaved, `repetitions` times.
#
# Writes results/${FAULTTWIN_RESULTS_PHASE:-phase-03}/collection/<label>/.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-${root}/build/release}"
reps="${2:-3}"
phase="${FAULTTWIN_RESULTS_PHASE:-phase-03}"
label="${FAULTTWIN_RESULTS_LABEL:-$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)}"
out="${root}/results/${phase}/collection/${label}"
monitor="${build_dir}/src/monitor/faulttwin-monitor"

[[ -x "${monitor}" ]] || { echo "error: ${monitor} not built" >&2; exit 2; }
[[ -f "${root}/.env" ]] || { echo "error: .env missing; run scripts/lab-init-env.sh" >&2; exit 2; }
set -a
# shellcheck disable=SC1091
. "${root}/.env"
set +a
cd "${root}"
docker compose up -d --wait >/dev/null
mkdir -p "${out}"

# Superuser access over the container's local socket (trusted inside the container only).
psql_on() { docker compose exec -T "$1" psql -X -q -U postgres -d postgres -At -c "$2"; }
nodes=(pg-primary pg-replica-1 pg-replica-2)

{
  echo "date_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "uname: $(uname -srm)"
  echo "cpus: $(getconf _NPROCESSORS_ONLN 2>/dev/null || echo unknown)"
  echo "docker: $(docker version --format '{{.Server.Version}}' 2>/dev/null || echo unknown)"
  echo "postgres: $(docker compose exec -T pg-primary postgres --version)"
  echo "monitor_build: ${build_dir}"
  echo "git_commit: $(git rev-parse HEAD 2>/dev/null || echo 'not a git repository')"
} > "${out}/environment.txt"

echo "==> A: idle cluster, 300 cycles at 200 ms"
for n in "${nodes[@]}"; do psql_on "${n}" "SELECT pg_stat_statements_reset()" >/dev/null; done
"${monitor}" --cycles 300 --interval-ms 200 --format json > "${out}/idle.jsonl"
echo "node,calls,total_exec_ms" > "${out}/idle-server.csv"
for n in "${nodes[@]}"; do
  row="$(psql_on "${n}" "SELECT coalesce(sum(calls),0), coalesce(round(sum(total_exec_time)::numeric, 3),0) FROM pg_stat_statements WHERE userid = 'ft_monitor'::regrole")"
  echo "${n},${row//|/,}" >> "${out}/idle-server.csv"
done

echo "==> B: pgbench throughput with and without the monitor (${reps} repetitions)"
echo "rep,condition,tps,latency_avg_ms" > "${out}/pgbench.csv"
run_pgbench() {
  docker compose exec -T pg-primary pgbench -n -U ft_workload -d postgres -c 4 -j 2 -T 15 \
    -f /lab/workload/insert.sql 2>&1
}
for rep in $(seq 1 "${reps}"); do
  for condition in none monitor-1000ms monitor-100ms; do
    monitor_pid=""
    case "${condition}" in
      monitor-1000ms) "${monitor}" --cycles 1000000 --interval-ms 1000 --format json > "${out}/pgbench-${rep}-${condition}.jsonl" & monitor_pid=$! ;;
      monitor-100ms)  "${monitor}" --cycles 1000000 --interval-ms 100 --format json > "${out}/pgbench-${rep}-${condition}.jsonl" & monitor_pid=$! ;;
    esac
    result="$(run_pgbench)"
    if [[ -n "${monitor_pid}" ]]; then kill "${monitor_pid}"; wait "${monitor_pid}" 2>/dev/null || true; fi
    echo "${result}" > "${out}/pgbench-${rep}-${condition}.txt"
    tps="$(echo "${result}" | sed -n 's/^tps = \([0-9.]*\) .*/\1/p')"
    lat="$(echo "${result}" | sed -n 's/^latency average = \([0-9.]*\) ms/\1/p')"
    echo "${rep},${condition},${tps},${lat}" >> "${out}/pgbench.csv"
    echo "    rep ${rep} ${condition}: ${tps} tps"
  done
done

# Per-cycle JSON is large and repetitive; keep it, compressed.
gzip -nf "${out}"/*.jsonl
python3 -I "${root}/scripts/summarize-collection.py" "${out}" | tee "${out}/summary.md"
