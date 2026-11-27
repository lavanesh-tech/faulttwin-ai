# Phase 4 raw evidence

Everything in this directory was produced by the AI implementation assistant in one Linux cloud
container (Ubuntu 24.04, Linux 6.18 x86_64, 4 vCPUs, GCC 13.3.0, Clang 18.1.3). None of it comes
from the owner's Mac or from GitHub Actions, and none of it is independently verified.

| Directory | Contents | Produced by |
|---|---|---|
| `gcc/`, `clang/` | `environment.txt` and full configure, build and test logs for debug, release, asan-ubsan and tsan | `scripts/verify-presets.sh gcc`, `scripts/verify-presets.sh clang` |
| `integration/` | `ctest -L integration -V` output with `FT_INTEGRATION=1` against the running lab, one file per build | `scripts/run-integration-tests.sh <build-dir>` |
| `detection/linux-x86_64-container/` | `faulttwin-detect evaluate` outputs: `summary.json`, `summary.md`, tuning grids of the rules, naive thresholds, k-NN and perceptron, alerts per test split, console output and an environment record | `build/rel/src/detect/faulttwin-detect evaluate --out …` (GCC Release) |
| `benchmarks/{gcc,clang}/` | Google Benchmark JSON (5 repetitions), console output, environment | `scripts/run-detection-benchmarks.sh gcc` / `clang` |

## Notes

- **Synthetic data.** Detection results come from deterministic synthetic scenarios with ground
  truth. They say nothing about real PostgreSQL faults (Phase 6 produces those datasets).
- **Determinism check.** The Clang Release build and the GCC Debug build were run with the same
  arguments and every output file was byte-identical to the committed GCC Release results
  (`cmp`). CI repeats this check with a debug build.
- **`git_commit` in the `environment.txt` files.** The container tracked its own edits in a
  throw-away local git repository, so a recorded commit id there (`571a8e0…`) is **not** a
  commit of the owner's repository. The code measured is the owner's `8f1dbb2` plus the
  uncommitted Phase 4 changes.
