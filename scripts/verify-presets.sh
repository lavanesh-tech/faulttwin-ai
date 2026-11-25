#!/usr/bin/env bash
# Configure, build and test every preset with one compiler, saving full logs as
# raw evidence under results/phase-01/<compiler>/.
#
# Usage: scripts/verify-presets.sh [gcc|clang] [preset ...]
# Default presets: debug release asan-ubsan tsan
set -euo pipefail

compiler="${1:-gcc}"
shift || true
presets=("$@")
if [[ ${#presets[@]} -eq 0 ]]; then
  presets=(debug release asan-ubsan tsan)
fi

case "${compiler}" in
  gcc) cc=gcc; cxx=g++ ;;
  clang) cc=clang; cxx=clang++ ;;
  *) echo "unknown compiler '${compiler}' (expected gcc or clang)" >&2; exit 2 ;;
esac

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${root}/results/phase-01/${compiler}"
mkdir -p "${out}"

{
  echo "date_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "uname: $(uname -srmo)"
  echo "cpu: $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ //' || echo unknown)"
  echo "cpus: $(nproc 2>/dev/null || echo unknown)"
  echo "compiler: $(${cxx} --version | head -1)"
  echo "cmake: $(cmake --version | head -1)"
  echo "ninja: $(ninja --version 2>/dev/null || echo missing)"
  echo "git_commit: $(git -C "${root}" rev-parse HEAD 2>/dev/null || echo 'not a git repository')"
} > "${out}/environment.txt"

status=0
for preset in "${presets[@]}"; do
  build_dir="${root}/build/${preset}-${compiler}"
  log="${out}/${preset}.log"
  echo "==> ${compiler} / ${preset}"

  # Mirror the environment of the matching test preset in CMakePresets.json.
  env_vars=()
  case "${preset}" in
    asan-ubsan)
      env_vars=(ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1:detect_stack_use_after_return=1
                UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1) ;;
    tsan)
      env_vars=(TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1) ;;
  esac

  if (
    set -x
    cd "${root}"
    cmake --preset "${preset}" -B "${build_dir}" -DCMAKE_C_COMPILER="${cc}" -DCMAKE_CXX_COMPILER="${cxx}"
    cmake --build "${build_dir}"
    env "${env_vars[@]}" ctest --test-dir "${build_dir}" --output-on-failure --no-tests=error
  ) > "${log}" 2>&1; then
    echo "    PASS  ($(grep -E 'tests passed' "${log}" | tail -1))"
  else
    echo "    FAIL  (see ${log})"
    status=1
  fi
done
exit "${status}"
