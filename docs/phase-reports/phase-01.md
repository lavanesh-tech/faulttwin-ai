# Phase 1 Implementation Report — C++20 Engineering Foundation

Date: 2026-11-25 (updated after the owner's macOS run and first commit) · Prepared for independent
technical review · Status: **In review, not complete.**

Terminology: *implemented* = code exists; *tested* = automated tests ran and passed;
*measured* = a metric was recorded under a protocol; *independently verified* = someone other than
the implementer reproduced it. In Phase 1, nothing is measured or independently verified.

---

## 1. Phase objective
Create a reproducible C++20 foundation: CMake project and presets, strict warnings, separate
sanitizer builds, pinned GoogleTest, the first domain type (telemetry value model) with explicit
validation contracts, a synthetic CLI demonstration, deterministic unit tests, a Linux CI workflow
and the documentation/evaluation scaffolding. Phase 0 design documents were reconciled with the
master specification at the same time.

## 2. Business problem addressed
Every later capability (detection, prediction, simulation, recovery advice) is only as trustworthy
as its input. Telemetry that is malformed, stale or ambiguous (for example, "no lag measurement"
silently read as "zero lag") produces wrong alerts and unsafe recovery decisions. Phase 1 makes
those cases explicit and testable at the type level. It does not by itself improve any business
metric.

## 3. Architecture decisions
- **ADR-0010** Two types: untrusted `RawTelemetrySample` and `TelemetrySample`, which only
  `validate_sample()` can construct. Errors are returned as a list of `{Field, ValidationCode}`
  issues (at most one per field, in field order), never thrown.
- **ADR-0010** Replication lag is `std::optional<double>`; `ReplicationLagState` distinguishes
  not-applicable / not-measured / measured. A primary or unknown-role node reporting any lag
  (even 0) is invalid.
- **ADR-0010** Validation never reads a clock: `now` is a parameter. Unset (≤ epoch) and future
  (> now + 5 s skew, configurable) timestamps are invalid; old samples are valid but *stale*,
  classified separately by `classify_freshness`.
- **ADR-0011** Presets `debug`, `release`, `asan-ubsan`, `tsan` in separate build trees; strict
  warnings + `-Werror` per first-party target only; sanitizer flags global so GoogleTest is
  instrumented too.
- **ADR-0004 (revised)** GoogleTest via FetchContent pinned to a commit SHA; vcpkg deferred to
  Phase 5 when heavy dependencies arrive.
- CLI option parsing and report formatting are a small library (`faulttwin_cli_lib`) so they can be
  unit tested; `main.cpp` only wires them together.
- No `src/core` module yet: nothing in Phase 1 needed it, and the specification forbids empty
  placeholder modules. The earlier Phase 1 draft's 42 placeholder directories were removed.

## 4. Files created and modified
The repository was empty, so every file is new.

| Path | Purpose |
|---|---|
| `CMakeLists.txt`, `CMakePresets.json` | Root build, options, presets |
| `cmake/CompilerWarnings.cmake`, `cmake/Sanitizers.cmake`, `cmake/Dependencies.cmake` | Warnings, sanitizers, pinned GoogleTest |
| `include/faulttwin/telemetry/{node_id,node_role,telemetry_sample,validation,synthetic}.hpp` | Public API |
| `src/CMakeLists.txt`, `src/telemetry/{CMakeLists.txt,node_id.cpp,node_role.cpp,validation.cpp,synthetic.cpp}` | Library |
| `src/cli/{CMakeLists.txt,options.hpp,options.cpp,report.hpp,report.cpp,main.cpp}` | `faulttwin-synth` |
| `tests/CMakeLists.txt`, `tests/unit/*_test.cpp` (6 files), `tests/cli/check_deterministic.cmake` | Tests |
| `scripts/verify-presets.sh` | Reproduces all builds/tests and saves logs |
| `results/phase-01/{gcc,clang}/*.log`, `environment.txt`, `README.md` | Raw evidence |
| `.github/workflows/ci.yml` | Linux CI |
| `.clang-format`, `.clang-tidy`, `.editorconfig`, `.gitignore` | Tooling config |
| `README.md` | Project README |
| `docs/PROGRESS.md` | Progress tracker |
| `docs/architecture/{requirements,overview,acceptance-criteria,dependencies}.md` | Phase 0 design |
| `docs/adr/0000`–`0011` | ADRs (0004 revised, 0008 revised, 0010 and 0011 new) |
| `docs/evaluation/{metric-definitions,baselines,experiment-protocols,metric-targets,verified-results,limitations}.md` | Evaluation plan (no results) |
| `docs/resume/{evidence-ledger,resume-bullets}.md` | Ledger (all PLANNED) and templates |
| `docs/phase-reports/phase-01.md` | This report |

Size: about 2,050 lines of C++ (headers, sources and tests).

## 5. Dependencies introduced and justification
| Dependency | Pin | Why |
|---|---|---|
| GoogleTest | v1.17.0, commit `52eb8108c5bdec04579160ae17225d66034bd723` | Required by the specification; test-only, not linked into shipped targets. GMock disabled (not needed). |
| actions/checkout, actions/upload-artifact | commit SHAs (v4.4.0, v4.6.2) | CI only; SHA pinning prevents a moved tag from changing CI behaviour. |

No runtime third-party dependency. Drogon, Boost.Asio, gRPC, libpq, ONNX Runtime and OpenAI were
deliberately **not** introduced (specification constraint).

## 6. Algorithms and data structures
- **Validation:** a fixed sequence of independent field checks; each pushes at most one issue into a
  `std::vector<ValidationIssue>`.
- **Node id check:** single linear scan with explicit ASCII classification.
- **Synthetic generator:** SplitMix64 PRNG (in-tree, fully specified); uniform doubles from the top
  53 bits; tick-major generation (each node once per tick, like a scrape cycle). All random draws
  happen unconditionally so the stream does not depend on which branch ran.
- **CLI summary:** `std::map<std::string, uint64_t>` for issue counts, so output order is stable.
- **Timestamp formatting:** C++20 `<chrono>` calendar types (`year_month_day`, `hh_mm_ss`) and
  `std::to_chars`, no `printf`-family varargs and no locale dependence.

## 7. Time and space complexity
| Operation | Time | Space |
|---|---|---|
| `check_node_id` | O(L), L ≤ 64 | O(1) |
| `validate_sample` | O(L) | O(1) beyond the result: ≤ 7 issues, plus one `NodeId` copy |
| `classify_freshness`, `check_*` numeric | O(1) | O(1) |
| `generate_synthetic_samples` | O(N·S) | O(N·S) samples held in a vector (bounded by CLI limits 64 × 100,000) |
| CLI run | O(N·S) | O(N·S) for samples + O(K) issue-type counters, K ≤ 11 |

## 8. Memory ownership decisions
- Everything is held by value; no heap ownership beyond `std::string` and `std::vector`. No raw
  owning pointers, no smart pointers (none were needed — Rule of Zero throughout).
- `TelemetrySample` and `NodeId` are immutable after construction and own their data.
- `ValidationResult` owns an optional sample and its issue list; it is returned by value (moved).
- `std::string_view` is used only for parameters that do not outlive the call.

## 9. Thread-safety considerations
- All validation and formatting functions are pure (no shared mutable state, no globals, no clock
  reads), so they are safe to call concurrently on different inputs.
- `TelemetrySample` is immutable, so sharing a const instance across threads is safe.
- There is no concurrency in Phase 1. The TSan build therefore verifies the configuration and
  catches nothing concurrency-specific yet; it becomes meaningful in Phase 2.

## 10. Failure scenarios handled
Empty, over-long, badly-starting or illegal-character node ids (including embedded NUL and
non-ASCII bytes); out-of-range enum values; NaN, ±∞ and out-of-range percentages (with exact
`nextafter` boundary tests); unset and future timestamps; negative or non-finite lag; lag reported
for a primary/unknown role; missing lag on a replica (accepted, flagged as not measured); stale but
valid samples; malformed CLI arguments (non-numeric, trailing junk, overflow, NaN/∞, out of range,
missing value, unknown flag); output-stream failure; unexpected exceptions in `main` (caught,
reported, distinct exit code).

## 11. Security considerations
- All input is treated as untrusted; validation is total (never crashes, never throws on bad data).
- Node ids are restricted to a safe ASCII set, which keeps them safe for later use as metric labels,
  log fields and file names. A path-traversal-style id (`pg-replica-1/../etc`) is a tested defect.
- CLI output escapes untrusted strings (`json_quote`), so control characters in input cannot
  corrupt terminals or JSON consumers.
- CLI sizes are capped (≤ 64 nodes, ≤ 100,000 samples per node) to bound memory.
- No secrets, credentials or network access in the product code. CI has `contents: read`
  permissions only, and third-party actions are pinned to SHAs.

## 12. Actual build commands
Run by the assistant in an ephemeral cloud container: Linux 6.18 x86_64, Intel Xeon @ 2.10 GHz,
4 vCPUs, Ubuntu 24.04 toolchain (GCC 13.3.0, Clang 18.1.3, CMake 3.28.3, Ninja 1.11.1). No git
repository was present there.

```bash
scripts/verify-presets.sh gcc     # for each preset: cmake --preset P -B build/P-gcc \
                                  #   -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ && cmake --build build/P-gcc
scripts/verify-presets.sh clang   # same with clang/clang++
cmake --preset debug && cmake --build --preset debug   # plain README flow
```

## 13. Actual test commands
```bash
ctest --test-dir build/<preset>-<compiler> --output-on-failure --no-tests=error   # via the script
ctest --preset debug
find include src tests -name '*.cpp' -o -name '*.hpp' | xargs clang-format --dry-run --Werror
find src -name '*.cpp' | xargs clang-tidy -p build/debug-clang --quiet --warnings-as-errors='*'
```

## 14. Test results
| Platform / compiler | debug | release | asan-ubsan | tsan | Run by |
|---|---|---|---|---|---|
| Linux x86_64 / GCC 13.3.0 | 57/57 pass | 57/57 pass | 57/57 pass | 57/57 pass | assistant (cloud container) |
| Linux x86_64 / Clang 18.1.3 | 57/57 pass | 57/57 pass | 57/57 pass | 57/57 pass | assistant (cloud container) |
| macOS arm64 / AppleClang 21.0.0 | 57/57 pass | NOT RUN | NOT RUN | NOT RUN | owner (own Mac) |

macOS build warnings seen by the owner, both outside first-party code:
- `-Wcharacter-conversion` inside GoogleTest's `gtest-printers.h` (`char8_t` to `char32_t`). This is
  third-party code, so our `-Werror` does not apply. Left as is; revisit when GoogleTest is upgraded.
- `ld: warning: ignoring duplicate libraries: libfaulttwin_telemetry.a`. The test executable listed
  `faulttwin::telemetry` and also got it transitively through `faulttwin_cli_lib`. **Fixed** in
  `tests/CMakeLists.txt` by linking only `faulttwin_cli_lib`. After the fix all eight Linux
  configurations were re-run and passed 57/57. The fix has **not** yet been re-run on macOS.

The 57 CTest tests are 52 GoogleTest cases in 8 suites (NodeId 7, NodeRole 4, Validation 12,
Freshness 2, PercentageCheck 10 parameterized boundary cases, Synthetic 8, CliOptions 5,
CliReport 4) plus 5 CLI tests that run the real executable (help, unknown option rejected,
out-of-range fraction rejected, ground truth matches on 2,000 samples with 30% defects,
byte-identical output for the same seed and different output for a different seed).

- clang-format 18.1.3: clean.
- clang-tidy 18.1.3 (committed `.clang-tidy`, warnings as errors): exit 0 on all first-party
  sources. Findings fixed along the way: function cognitive complexity in option parsing (split into
  helpers), unchecked `optional` access, C varargs (`snprintf` replaced by `std::to_chars`), an
  unused using-declaration, a raw-string suggestion, and a redundant member initializer.
  `misc-include-cleaner` is disabled (see §21). clang-tidy must use a Clang compile database; with
  GCC's it errors on GCC-only warning flags (CI uses Clang).
- Test sources were not run through clang-tidy.

Coverage: **NOT RUN** (no coverage preset yet).

## 15. Sanitizer results
- ASan+UBSan (`-fno-sanitize-recover=all`, leak detection on): 57/57 pass with GCC and Clang, no
  reports.
- TSan: 57/57 pass with GCC and Clang, no reports. (No threads exist yet.)
- Sanity check that the sanitizers are live: a deliberate out-of-bounds read compiled with the same
  flags produced `AddressSanitizer: heap-buffer-overflow` with both compilers.
- First Clang sanitizer attempt **failed to link** because the container lacked the compiler-rt
  runtime. After `apt-get install libclang-rt-18-dev`, both passed. Failed logs are kept as
  `results/phase-01/clang/*.first-attempt-missing-runtime.log`; CI installs that package.

## 16. Benchmark results
Not applicable in Phase 1. **No benchmarks were run.** Google Benchmark is not introduced yet.

## 17. Baseline comparison
Not applicable in Phase 1.

## 18. Raw evidence artifact locations
`results/phase-01/gcc/` and `results/phase-01/clang/`: `environment.txt` and one full log per preset
(configure, build and test output). Produced by the assistant in the cloud container described in
§12 (latest run is after the link fix). `results/phase-01/macos/appleclang-debug.log`: the owner's
own macOS run, transcribed from their terminal output (per-test lines abbreviated). Nothing is
independently reproduced by a third party.

## 19. Updated evidence ledger
`docs/resume/evidence-ledger.md`: every metric row is `PLANNED`. A Phase 1 functional table records
validation, determinism and sanitizer cleanliness as `TESTED`, the macOS debug build as `TESTED`
(owner-run), and the CI run as `NOT RUN`. `docs/evaluation/verified-results.md` intentionally contains no results.

## 20. Git status and commit SHA
- **Commit `3223488`** (root commit on `main`, pushed by the owner): 75 files, authored by the owner
  only. The assistant ran no git commands: no commit, push, branch or config change.
- Not in that commit: `.github/workflows/ci.yml`. The assistant's file tool cannot write into
  `.github/`, so the file was placed at the repository root as `ci.yml` for the owner to move.
- Uncommitted working-tree changes after `3223488` (for the owner to review and commit):
  `ci.yml` (to be moved to `.github/workflows/ci.yml`), `tests/CMakeLists.txt` (duplicate-link fix),
  `results/phase-01/macos/appleclang-debug.log` (new), refreshed `results/phase-01/{gcc,clang}/` logs,
  and documentation updates (`docs/phase-reports/phase-01.md`, `docs/PROGRESS.md`,
  `docs/architecture/acceptance-criteria.md`, `docs/resume/evidence-ledger.md`,
  `docs/evaluation/limitations.md`, `results/phase-01/README.md`).
- No file names Claude, Anthropic or any AI as author or co-author. The README's "Development process
  and AI assistance" paragraph is a draft disclosure for the owner to edit.

Suggested message for the follow-up commit (for the owner to review and commit):
```
ci: add GitHub Actions workflow; fix duplicate test link; record macOS run

- Add .github/workflows/ci.yml (gcc/clang x 4 presets, clang-format, clang-tidy)
- Link unit tests only against faulttwin_cli_lib (telemetry comes transitively),
  removing Apple ld "ignoring duplicate libraries" warning
- Record owner's macOS AppleClang 21 debug run (57/57) as Phase 1 evidence
- Update Phase 1 report, progress tracker, acceptance criteria and ledger
```

## 21. Known bugs and limitations
- The GitHub Actions workflow has **never run**. Likely first-run risks: package names on the runner
  image and the `vm.mmap_rnd_bits` TSan workaround.
- macOS was built and tested only in the debug preset with AppleClang 21. Release and sanitizer
  presets on macOS have not been run, and the duplicate-link fix has not been re-run there. Older
  Xcode versions may lack `std::to_chars` for `double` with a precision argument.
- GoogleTest v1.17.0 emits a `-Wcharacter-conversion` warning under AppleClang 21 (third-party).
- `misc-include-cleaner` is disabled. It flagged about 60 missing direct includes, mostly
  "include what you use" style; a few were real and were fixed.
- Synthetic values are uniform within fixed ranges and do not model real load. A defect can change
  a node's role (e.g. `pg-primary` reported as `replica` for the negative-lag defect), which is
  deliberate for the test but can look odd in output.
- No cap on replication lag. This is deliberate, because a very large lag is real data.
- `max_future_skew` is trusted configuration, so overflow of `now + skew` is not guarded.
- Coverage is not measured.

## 22. Incomplete requirements
| Requirement | Status |
|---|---|
| Linux GitHub Actions workflow — *runs green* | Written, NOT RUN |
| Owner-machine build (macOS) | Debug preset 57/57 (owner-run); other presets NOT RUN |
| Coverage measurement (target ≥ 85% is a later-phase target) | NOT RUN |
| Licence | Owner decision pending |

Everything else in the Phase 1 deliverable list is implemented and tested on Linux.

## 23. Questions requiring architecture review
1. Should the `kUnknown` role be valid (current choice: yes, with lag forbidden) or rejected until
   topology is known?
2. Is rejecting lag on a primary, even 0, the right contract? The alternative is to accept and
   ignore it.
3. Is a 5 s default future-skew tolerance appropriate, or should it come from measured collector
   clock drift in Phase 3?
4. Validation reports one issue per field. Is that enough, or should a field be able to report
   several issues?
5. Should `src/core` be created now (for shared result/clock types), or wait until Phase 2 needs it
   (current choice)?
6. Should macOS (Apple Clang) be added to the CI matrix?

## 24. Practical learning exercises
1. Add a `network_latency_ms` optional field end to end: struct, `Field`, check, string form, tests,
   and a synthetic defect. Count how many places change and consider why.
2. Change `check_percentage` to accept 100.0000001 and watch which tests fail. What does
   `std::nextafter` buy you in boundary tests?
3. Replace `SplitMix64` with `std::mt19937_64` + `std::uniform_real_distribution` and build with
   GCC and with Clang + libc++. Do the outputs match? Relate the result to ADR-0007.
4. Remove `-fno-sanitize-recover=all`, introduce a signed overflow in a test, and compare the
   behaviour with and without it.
5. Try to construct a `TelemetrySample` from outside `validation.cpp` and read the compiler error.
   Explain how the `detail::SampleFactory` friend enforces the invariant, and what it does *not*
   protect against.
6. Run `faulttwin-synth --format jsonl` and pipe it into `jq` to count issue types. Then try to
   break `json_quote` with a crafted node id.

## 25. Interview questions with explanations
1. **Why is lag `std::optional<double>` and not `double` with -1 for missing?** A sentinel lives in
   the same domain as real values and gets averaged, compared and plotted by mistake. `optional`
   forces every consumer to handle "not measured" explicitly at compile time.
2. **Why can't callers construct `TelemetrySample` directly?** It makes "validated" a type-level
   property ("parse, don't validate"), so downstream code never re-checks and can't forget to check.
3. **Why does validation take `now` as a parameter?** It makes it deterministic and testable (no
   hidden clock), and replay can validate historical data against historical time.
4. **Stale vs invalid?** Invalid data is wrong and must not be used. Stale data is correct but old,
   and may be used with care or flagged. Merging the two either discards real data or trusts
   outdated data.
5. **Why not `std::isalnum`?** It is locale-dependent, and passing a negative `char` (any non-ASCII
   byte on signed-char platforms) is undefined behaviour.
6. **Why are ASan and TSan separate builds?** Their runtimes and shadow-memory layouts are
   incompatible. Sanitizer builds also distort performance, so they are never used for benchmarks.
7. **Why apply warnings per target instead of globally?** `-Werror` on third-party code means an
   upstream release or compiler upgrade can break your build for code you don't own.
8. **Why pin GoogleTest to a commit SHA rather than a tag?** Tags can be moved; a SHA is
   content-addressed, so the build is reproducible.
9. **Why an in-tree PRNG instead of `<random>` distributions?** The engines are specified, but the
   distributions' algorithms are implementation-defined, so the same seed gives different sequences
   on libstdc++ and libc++.
10. **Why collect all validation issues instead of failing fast?** Operators and tests see every
    problem at once, and the output is a stable, ordered list that is easy to assert on.

## 26. Phase acceptance criteria and current status
From `docs/architecture/acceptance-criteria.md`:

| # | Criterion | Status |
|---|---|---|
| 1.1 | Root CMake + presets; in-source builds rejected | ✅ implemented, tested |
| 1.2 | C++20 library and executable targets | ✅ |
| 1.3 | GoogleTest pinned to a commit | ✅ |
| 1.4 | Debug and Release pass with GCC and Clang (Linux) | ✅ tested |
| 1.5 | ASan+UBSan and TSan pass with GCC and Clang (Linux) | ✅ tested |
| 1.6 | Strict first-party warnings, warning-free build | ✅ |
| 1.7 | Telemetry model has all required fields | ✅ |
| 1.8 | Deterministic tests for every validation case | ✅ tested |
| 1.9 | Missing lag distinguishable from zero lag | ✅ tested |
| 1.10 | CLI deterministic and self-checking | ✅ tested |
| 1.11 | clang-format and clang-tidy clean | ✅ |
| 1.12 | CI workflow exists | ✅ |
| 1.13 | CI workflow green on GitHub | ⏳ NOT RUN |
| 1.14 | Builds on owner's macOS | ✅ debug preset, 57/57, AppleClang 21 (owner-run) |
| 1.15 | Docs, ADRs, metric definitions, plan, empty ledger | ✅ |
| 1.16 | No AI authorship; no assistant commits | ✅ |
| 1.17 | Owner approval after independent review | ⏳ |

**Phase 1 is not complete.** 1.13 needs `ci.yml` committed and a green CI run; 1.17 needs your
review and approval.
