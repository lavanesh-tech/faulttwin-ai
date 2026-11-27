#!/usr/bin/env bash
# Builds the Release preset with one compiler, runs the Phase 4 feature/detection benchmarks
# (5 repetitions each) and writes raw JSON plus an environment record to
# results/phase-04/benchmarks/<compiler>/.
#
# Usage: scripts/run-detection-benchmarks.sh [gcc|clang]
set -euo pipefail

compiler="${1:-gcc}"
case "${compiler}" in
  gcc) cc=gcc; cxx=g++ ;;
  clang) cc=clang; cxx=clang++ ;;
  *) echo "unknown compiler '${compiler}' (expected gcc or clang)" >&2; exit 2 ;;
esac

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${root}/build/release-${compiler}"
out="${root}/results/phase-04/benchmarks/${compiler}"
mkdir -p "${out}"

cd "${root}"
cmake --preset release -B "${build_dir}" -DCMAKE_C_COMPILER="${cc}" -DCMAKE_CXX_COMPILER="${cxx}"
cmake --build "${build_dir}" --target faulttwin_detection_bench

{
  echo "date_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "uname: $(uname -srm)"
  if [[ -r /proc/cpuinfo ]]; then
    echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
    echo "cpus: $(nproc)"
  else
    echo "cpu: $(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)"
    echo "cpus: $(sysctl -n hw.ncpu 2>/dev/null || echo unknown)"
  fi
  echo "load_average_before: $(uptime | sed 's/.*load average[s]*: //')"
  echo "compiler: $(${cxx} --version | head -1)"
  echo "build_type: Release"
  echo "git_commit: $(git -C "${root}" rev-parse HEAD 2>/dev/null || echo 'not a git repository')"
  echo "git_uncommitted_files: $(git -C "${root}" status --porcelain 2>/dev/null | wc -l | tr -d ' ')"
} > "${out}/environment.txt"
cat "${out}/environment.txt"

"${build_dir}/bench/faulttwin_detection_bench" \
  --benchmark_repetitions=5 --benchmark_report_aggregates_only=false \
  --benchmark_out="${out}/detection.json" --benchmark_out_format=json \
  | tee "${out}/detection.console.txt"
