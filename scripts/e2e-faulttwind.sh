#!/usr/bin/env bash
# End-to-end check of faulttwind over real HTTP.
#
# Usage: scripts/e2e-faulttwind.sh BINARY PORT [extra faulttwind options...]
#
# Starts faulttwind on 127.0.0.1:PORT with the synthetic source (unless the extra options say
# otherwise), waits until /readyz is ready, checks the public endpoints, acknowledges one
# incident, then sends SIGTERM and requires a clean exit (status 0). FT_API_KEY is generated
# for the run if it is not set. Needs curl and python3.
#
# With --store postgres or --source postgres the run needs the lab (FT_INTEGRATION=1 and the
# .env variables loaded); without FT_INTEGRATION=1 it exits 77, which CTest reports as skipped.
set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "usage: $0 BINARY PORT [faulttwind options...]" >&2
  exit 2
fi
bin="$1"
port="$2"
shift 2
base="http://127.0.0.1:${port}"
if [[ " $* " == *" postgres "* && "${FT_INTEGRATION:-}" != "1" ]]; then
  echo "SKIP: set FT_INTEGRATION=1 with the lab running for a postgres e2e run"
  exit 77
fi
if [[ -z "${FT_API_KEY:-}" ]]; then
  FT_API_KEY="$(python3 -c 'import secrets; print(secrets.token_hex(16))')"
  export FT_API_KEY
fi

log="$(mktemp)"
"${bin}" --listen "127.0.0.1:${port}" --interval-ms 20 "$@" > "${log}" 2>&1 &
pid=$!
cleanup() {
  if kill -0 "${pid}" 2>/dev/null; then
    kill -KILL "${pid}" 2>/dev/null || true
  fi
  rm -f "${log}"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  echo "--- faulttwind log ---" >&2
  cat "${log}" >&2
  exit 1
}

status_of() {
  curl -s -o /dev/null -w '%{http_code}' "$@"
}

ready=0
for _ in $(seq 1 100); do
  if [[ "$(status_of "${base}/readyz")" == "200" ]]; then
    ready=1
    break
  fi
  kill -0 "${pid}" 2>/dev/null || fail "faulttwind exited during startup"
  sleep 0.1
done
[[ ${ready} -eq 1 ]] || fail "/readyz did not become ready"

[[ "$(status_of "${base}/healthz")" == "200" ]] || fail "/healthz"
curl -s -D - -o /dev/null "${base}/healthz" | grep -qi '^x-request-id:' || fail "X-Request-Id header"
[[ "$(status_of "${base}/api/v1/version")" == "200" ]] || fail "/api/v1/version"
[[ "$(status_of "${base}/api/v2/version")" == "404" ]] || fail "/api/v2 should be 404"
[[ "$(status_of "${base}/api/v1/incidents?bogus=1")" == "400" ]] || fail "unknown query parameter"
[[ "$(status_of -X DELETE "${base}/api/v1/incidents")" == "405" ]] || fail "DELETE should be 405"
curl -s "${base}/metrics" | grep -q '^faulttwin_monitor_cycles_total' || fail "/metrics"

nodes="$(curl -s "${base}/api/v1/nodes")"
python3 -c 'import json,sys; d=json.loads(sys.argv[1]); assert len(d["nodes"])>=1' "${nodes}" \
  || fail "/api/v1/nodes: ${nodes}"

id=""
for _ in $(seq 1 100); do
  id="$(curl -s "${base}/api/v1/incidents?limit=1" |
    python3 -c 'import json,sys; i=json.load(sys.stdin)["items"]; print(i[0]["id"] if i else "")')"
  [[ -n "${id}" ]] && break
  sleep 0.1
done
[[ -n "${id}" ]] || fail "no incident recorded"
[[ "$(status_of "${base}/api/v1/incidents/${id}")" == "200" ]] || fail "GET incident ${id}"

ack_url="${base}/api/v1/incidents/${id}/acknowledge"
body='{"by":"e2e","note":"checked"}'
[[ "$(status_of -X POST -H 'Content-Type: application/json' -d "${body}" "${ack_url}")" == "401" ]] \
  || fail "acknowledge without a token should be 401"
[[ "$(status_of -X POST -H "Authorization: Bearer ${FT_API_KEY}" \
  -H 'Content-Type: application/json' -d "${body}" "${ack_url}")" == "200" ]] \
  || fail "acknowledge"
[[ "$(status_of -X POST -H "Authorization: Bearer ${FT_API_KEY}" \
  -H 'Content-Type: application/json' -d "${body}" "${ack_url}")" == "409" ]] \
  || fail "second acknowledge should be 409"

kill -TERM "${pid}"
code=0
wait "${pid}" || code=$?
[[ ${code} -eq 0 ]] || fail "faulttwind exited with ${code} after SIGTERM"
echo "PASS: faulttwind e2e on port ${port} ($*)"
