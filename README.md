# FaultTwin AI

**AI-Powered Distributed Database Failure Prediction and Recovery Engine** — an experimental
reliability-engineering platform, written primarily in C++20, that will collect PostgreSQL cluster
telemetry, detect anomalies, predict developing failures, simulate recovery strategies on a custom
discrete-event digital twin and recommend evidence-based actions. It is advisory: it does not
execute destructive or production recovery actions.

> **Status: Phase 1 of 12 (engineering foundation).** What exists today is the build system, the
> telemetry value model with validation, a synthetic-data CLI and tests. Everything else is design.
> No performance, ML or business result has been measured. See [`docs/PROGRESS.md`](docs/PROGRESS.md).

## What works now

- `faulttwin_telemetry` library: bounded `NodeId`, `NodeRole`, `RawTelemetrySample` →
  `validate_sample()` → `TelemetrySample` (valid by construction), with explicit missing-vs-zero
  replication lag and a stale-vs-invalid distinction.
- `faulttwin-synth`: generates deterministic synthetic telemetry, injects known defects, validates
  every sample and checks the validator against the injected ground truth.
- 57 tests (GoogleTest + CLI tests), clean under ASan+UBSan and TSan with GCC and Clang on Linux.

## Build and test

Requirements: CMake ≥ 3.25, Ninja, GCC ≥ 13 or Clang ≥ 18, network access to github.com for the
first configure (GoogleTest is fetched at a pinned commit). Sanitizer builds with Clang on Ubuntu
also need `libclang-rt-18-dev`.

```bash
cmake --preset debug          # or: release, asan-ubsan, tsan
cmake --build --preset debug
ctest --preset debug

# Everything, with full logs saved under results/phase-01/<compiler>/
scripts/verify-presets.sh gcc
scripts/verify-presets.sh clang
```

## Try the CLI

```bash
./build/debug/src/cli/faulttwin-synth --nodes 3 --samples-per-node 2 --invalid-fraction 0.5 --seed 3
./build/debug/src/cli/faulttwin-synth --nodes 5 --samples-per-node 1000 --seed 7 \
    --invalid-fraction 0.2 --format jsonl --quiet
./build/debug/src/cli/faulttwin-synth --help
```

Exit status 0 means the validator agreed with the injected ground truth for every sample.

## Repository layout (current)

```
include/faulttwin/telemetry/   Public headers: node id, role, sample, validation, synthetic generator
src/telemetry/                 Library implementation
src/cli/                       faulttwin-synth (options + report are a small testable library)
tests/unit/                    GoogleTest suites
tests/cli/                     CLI determinism check (CMake script)
cmake/                         Warnings, sanitizers, pinned dependencies
scripts/verify-presets.sh      Configure/build/test every preset and save logs
results/phase-01/              Raw logs from the Phase 1 verification runs
docs/                          Architecture, ADRs, evaluation plan, evidence ledger, phase reports
.github/workflows/ci.yml       Linux CI: gcc/clang × 4 presets, clang-format, clang-tidy
```

Directories for later modules are created only when those modules are implemented.

## Documentation
- [Requirements and MVP boundaries](docs/architecture/requirements.md)
- [Architecture (target design)](docs/architecture/overview.md)
- [Acceptance criteria](docs/architecture/acceptance-criteria.md)
- [Dependencies](docs/architecture/dependencies.md)
- [ADRs](docs/adr/)
- [Metric definitions](docs/evaluation/metric-definitions.md), [baselines](docs/evaluation/baselines.md),
  [protocols](docs/evaluation/experiment-protocols.md), [targets](docs/evaluation/metric-targets.md),
  [verified results](docs/evaluation/verified-results.md) (none yet),
  [limitations](docs/evaluation/limitations.md)
- [Evidence ledger](docs/resume/evidence-ledger.md)
- [Phase 1 report](docs/phase-reports/phase-01.md)

## Development process and AI assistance

This project is designed, reviewed and owned by its sole author. An AI coding assistant is used for
implementation help, and every change is reviewed, built and committed by the author. Results are
reported only when measured.

## Licence

Not yet chosen. Third-party components keep their own licences (GoogleTest: BSD-3-Clause).
