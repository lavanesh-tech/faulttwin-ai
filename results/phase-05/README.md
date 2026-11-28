# Phase 5 raw evidence

Everything here was produced by the AI implementation assistant in one Linux cloud container:
- Ubuntu 24.04, Linux 6.18 x86_64, 4 vCPUs;
- GCC 13.3.0, Clang 18.1.3;
- Docker 29.9.3.

None of it comes from the owner's Mac or from GitHub Actions, and none of it is independently
verified.

| Directory | Contents | Produced by |
|---|---|---|
| `gcc/`, `clang/` | `environment.txt`, plus full configure, build and test logs for debug, release, asan-ubsan and tsan | `scripts/verify-presets.sh gcc`, `scripts/verify-presets.sh clang` |
| `integration/` | `ctest -L integration -V` output with `FT_INTEGRATION=1`, against the running lab and meta-db | `FAULTTWIN_RESULTS_LABEL=linux-x86_64-container scripts/run-integration-tests.sh build/debug` |
| `observability/` | Prometheus and Grafana API responses while `faulttwind` ran (one check) | commands written into the file |
| `build-cost/` | Wall time to configure, to build Drogon and Trantor, and to build the rest (one run) | commands in the phase report §16 |

## Notes
- **git_commit.** The `git_commit` in the environment files and logs is the container's
  throw-away tracking repository (scratch identity), not the owner's repository. The owner's base
  is `6209dee`, with Phase 5 uncommitted.
- **Integration tests in the preset logs.** In `gcc/` and `clang/`, CTest reports 23 of the 25
  integration tests as `Skipped`, because `FT_INTEGRATION` is unset. They are not failures. The
  real integration run is in `integration/`.
- **Grafana password.** Grafana API calls in `observability/` used the admin password from `.env`.
  It is shown as `***` and was never written to a file.
