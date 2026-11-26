# Phase 3 raw evidence

Everything in this directory was produced by the AI implementation assistant in one Linux cloud
container (Ubuntu 24.04.5, Linux 6.18 x86_64, 4 vCPUs, Docker 29.9.3, Compose v5.7.1). None of it
comes from the owner's Mac or from GitHub Actions, and none of it is independently verified.

| Directory | Contents | Produced by |
|---|---|---|
| `gcc/`, `clang/` | `environment.txt` and full configure, build and test logs for debug, release, asan-ubsan and tsan | `scripts/verify-presets.sh gcc`, `scripts/verify-presets.sh clang` |
| `integration/` | `ctest -L integration -V` output with `FT_INTEGRATION=1` against the running lab, one file per build | `scripts/run-integration-tests.sh <build-dir>` |
| `collection/linux-x86_64-container/` | idle collection run, pg_stat_statements cost of the monitor, pgbench with and without the monitor, `summary.md` | `scripts/measure-collection.sh build/release 3` |

## Notes

- **`git_commit` in the `environment.txt` files.** The container tracked its own edits in a
  throw-away local git repository, so `git_commit` shows that repository's commit id
  (`44a3ff8…`). It is **not** a commit of the owner's repository. The code measured is the owner's
  `f0624a4` plus the uncommitted Phase 3 changes. The raw value is kept as recorded; a note line
  was appended to each file.
- **Compressed JSON lines.** The monitor's per-cycle output (`*.jsonl`) was gzipped after the run to
  keep the repository small. `scripts/summarize-collection.py` reads `.jsonl` and `.jsonl.gz`
  alike; re-running it on the compressed files gives an identical `summary.md`.
- **Percentiles** are nearest-rank over the recorded samples. The first cycle (which opens the
  connections) is reported separately and excluded from the steady-state rows.
- **pgbench runs** were interleaved (none, 1000 ms, 100 ms, then repeat) so that slow drift of the
  shared host affects every condition alike. Three runs per condition cannot resolve differences
  below about 5 %.
