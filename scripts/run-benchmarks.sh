#!/usr/bin/env bash
# Builds the Release preset with one compiler, runs the Phase 2 benchmarks and
# writes raw JSON, an environment record and CSV/Markdown summaries to
# results/phase-02/benchmarks/<compiler>/.
#
# Usage: scripts/run-benchmarks.sh [gcc|clang] [--quick]
#   --quick  only a few configurations (each still has its built-in 5
#            repetitions); for checking the script, never for reporting.
set -euo pipefail

compiler="${1:-gcc}"
quick="${2:-}"
case "${compiler}" in
  gcc) cc=gcc; cxx=g++ ;;
  clang) cc=clang; cxx=clang++ ;;
  *) echo "unknown compiler '${compiler}' (expected gcc or clang)" >&2; exit 2 ;;
esac

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${root}/build/release-${compiler}"
out="${root}/results/phase-02/benchmarks/${compiler}"
if [[ "${quick}" == "--quick" ]]; then
  out="${out}-quick"
fi
mkdir -p "${out}"

cd "${root}"
cmake --preset release -B "${build_dir}" -DCMAKE_C_COMPILER="${cc}" -DCMAKE_CXX_COMPILER="${cxx}"
cmake --build "${build_dir}" --target faulttwin_bench
bench="${build_dir}/bench/faulttwin_bench"

# ---- Environment record (experiment-protocols.md §1) ------------------------
first_line() { "$@" 2>/dev/null | head -1 || echo unknown; }
if [[ -r /proc/cpuinfo ]]; then
  cpu_model="$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
  cpus="$(nproc)"
  mem_kib="$(grep -m1 MemTotal /proc/meminfo | awk '{print $2}')"
else
  cpu_model="$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)"
  cpus="$(sysctl -n hw.ncpu 2>/dev/null || echo unknown)"
  mem_kib="$(( $(sysctl -n hw.memsize 2>/dev/null || echo 0) / 1024 ))"
fi
cgroup_cpu="$(cat /sys/fs/cgroup/cpu.max 2>/dev/null || echo 'n/a')"
cgroup_mem="$(cat /sys/fs/cgroup/memory.max 2>/dev/null || echo 'n/a')"
if git -C "${root}" rev-parse HEAD >/dev/null 2>&1; then
  git_commit="$(git -C "${root}" rev-parse HEAD)"
  git_dirty="$(git -C "${root}" status --porcelain | wc -l | tr -d ' ')"
else
  git_commit="not a git repository"
  git_dirty="unknown"
fi
cxx_flags="$(grep -E '^CMAKE_CXX_FLAGS_RELEASE:' "${build_dir}/CMakeCache.txt" | cut -d= -f2-)"

cat > "${out}/environment.json" <<EOF
{
  "date_utc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "git_commit": "${git_commit}",
  "git_uncommitted_files": "${git_dirty}",
  "uname": "$(uname -srm)",
  "cpu_model": "${cpu_model}",
  "logical_cpus": "${cpus}",
  "mem_total_kib": "${mem_kib}",
  "cgroup_cpu_max": "${cgroup_cpu}",
  "cgroup_memory_max": "${cgroup_mem}",
  "load_average_before": "$(uptime | sed 's/.*load average[s]*: //')",
  "compiler": "$(first_line "${cxx}" --version)",
  "cmake": "$(first_line cmake --version)",
  "build_type": "Release",
  "cxx_flags_release": "${cxx_flags}",
  "google_benchmark": "v1.9.4 (eddb0241389718a23a42db6af5f0164b6e0139af)",
  "quick_mode": "$([[ "${quick}" == "--quick" ]] && echo true || echo false)"
}
EOF
cat "${out}/environment.json"

context=(--benchmark_context=git_commit="${git_commit}" --benchmark_context=compiler="${compiler}")
repetitions=()
main_filter='^BM_(SingleThreadedBaseline|Pipeline|PipelineBatchAblation|BoundedQueuePushPop|UnboundedQueuePushPop)/'
if [[ "${quick}" == "--quick" ]]; then
  repetitions=(--benchmark_repetitions=1)
  main_filter='^BM_(SingleThreadedBaseline/work:0|Pipeline/producers:1/consumers:1/capacity:1024/work:0)/'
fi

# ---- Experiment 1 + micro-benchmarks: one process ----------------------------
"${bench}" --benchmark_filter="${main_filter}" ${repetitions[@]+"${repetitions[@]}"} "${context[@]}" \
  --benchmark_out="${out}/ingest.json" --benchmark_out_format=json \
  | tee "${out}/ingest.console.txt"

# ---- Experiment 2 (reduced): one process per queue, so peak RSS is per design --
overload=("BM_OverloadBounded/policy:0/" "BM_OverloadBounded/policy:1/"
          "BM_OverloadBounded/policy:2/" "BM_OverloadUnbounded/")
names=(bounded-block bounded-reject-newest bounded-drop-oldest unbounded)
for i in "${!overload[@]}"; do
  "${bench}" --benchmark_filter="^${overload[$i]}" ${repetitions[@]+"${repetitions[@]}"} "${context[@]}" \
    --benchmark_out="${out}/overload-${names[$i]}.json" --benchmark_out_format=json \
    | tee "${out}/overload-${names[$i]}.console.txt"
done

python3 "${root}/scripts/summarize-benchmarks.py" "${out}"
