# ADR-0004: Dependency management — pinned FetchContent now, package manager when heavy dependencies arrive

- Status: Accepted for Phase 1; the Phase 5 part is Proposed
- Date: 2026-11-25
- Supersedes: the earlier draft "vcpkg manifest mode inside a pinned dev container"

## Context
Phase 1 needs exactly one third-party library: GoogleTest. Later phases add Drogon (Phase 5),
Protobuf/gRPC (Phase 5, if justified), libpq (Phase 3) and ONNX Runtime (Phase 7). Builds must be
reproducible on Linux CI and buildable on the owner's macOS machine.

## Decision
- **Phase 1:** GoogleTest via CMake `FetchContent`, pinned to the full commit SHA behind the
  `v1.17.0` tag (`52eb8108c5bdec04579160ae17225d66034bd723`), not to a moving tag or branch.
  GoogleTest is a test-only dependency and is never linked into shipped binaries.
- **Phase 5 (proposed):** when Drogon and other heavy libraries arrive, move to vcpkg manifest mode
  with a pinned `builtin-baseline`, decided in a new ADR with measured build times.

## Alternatives considered
- vcpkg now: adds a toolchain bootstrap step for a single small dependency.
- System packages (`libgtest-dev`): versions differ by distro release, breaking reproducibility.
- Vendoring the source: reproducible but bloats the repository and hides upgrades.

## Consequences
- The first configure needs network access to github.com; later configures reuse `build/_deps`.
- GoogleTest's own warnings are not subject to our `-Werror` (warnings are applied per first-party
  target only).

## Revisit when
Phase 5 introduces Drogon.
