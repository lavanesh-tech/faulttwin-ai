# ADR-0011: Build and test foundation — presets, per-target warnings, separate sanitizer trees

- Status: Accepted (Phase 1)
- Date: 2026-11-25

## Decision
- CMake ≥ 3.25 with `CMakePresets.json`: `debug`, `release`, `asan-ubsan`, `tsan`, each in its own
  build tree under `build/<preset>`. Ninja generator.
- C++20, extensions off, `compile_commands.json` exported for clang-tidy and IDEs.
- Strict warnings (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wold-style-cast …`, plus GCC-only `-Wduplicated-cond -Wduplicated-branches -Wlogical-op`) are
  applied **per first-party target** with `-Werror` on by default. Third-party code is excluded.
  `-Wuseless-cast` was tried and removed: it fires on casts that are needed on platforms where
  `std::size_t` and `std::uint64_t` differ.
- Sanitizer flags are global (`add_compile_options`) so GoogleTest is instrumented consistently.
  ASan+UBSan use `-fno-sanitize-recover=all` so any UB report fails the test. TSan is separate
  because it cannot be combined with ASan.
- Release builds are the only builds used for performance numbers.
- CI: GitHub Actions on `ubuntu-24.04`, matrix {gcc, clang} × {debug, release, asan-ubsan, tsan},
  plus clang-format and clang-tidy jobs. Third-party actions are pinned to commit SHAs.

## Consequences
- Eight build jobs per push; acceptable for a small codebase, revisit if CI time grows.
- macOS (Apple Clang) is not in CI yet; local macOS builds are best-effort.
