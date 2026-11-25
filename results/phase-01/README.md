# Phase 1 raw evidence

Full configure/build/test logs from `scripts/verify-presets.sh`, one directory per compiler, with an
`environment.txt` describing where the run happened.

These runs were executed by the AI implementation assistant in an ephemeral Linux x86_64 cloud
container, not on the owner's machine and not in GitHub Actions. They have **not** been
independently reproduced. Re-running the script overwrites them.

`*.first-attempt-missing-runtime.log` are the first Clang sanitizer attempts, which failed to link
because that container lacked `libclang-rt-18-dev`. They are kept to show the failure honestly; the
passing reruns are in `asan-ubsan.log` and `tsan.log`.
