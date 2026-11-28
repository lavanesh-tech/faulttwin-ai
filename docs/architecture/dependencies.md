# FaultTwin AI — Dependency Matrix

## Actually used (Phases 1–5)

| Dependency | Version / pin | Used for | Licence |
|---|---|---|---|
| CMake | ≥ 3.25 required; 3.28.3 used for the recorded runs | Build | BSD-3-Clause |
| Ninja | 1.11.1 used for the recorded runs | Generator | Apache-2.0 |
| GCC | 13.3.0 (Ubuntu 24.04) used for the recorded runs | Compiler | GPL (toolchain only) |
| Clang / clang-tidy / clang-format | 18.1.3 (Ubuntu 24.04) used for the recorded runs | Compiler, lint, format | Apache-2.0 with LLVM exception |
| GoogleTest | v1.17.0, commit `52eb8108c5bdec04579160ae17225d66034bd723` via FetchContent | Unit tests only | BSD-3-Clause |
| Google Benchmark | v1.9.4, commit `eddb0241389718a23a42db6af5f0164b6e0139af` via FetchContent (Phase 2, ADR-0013) | `bench/` only, Release preset only | Apache-2.0 |
| Python 3 (standard library only) | 3.13 used for the recorded runs | `scripts/summarize-benchmarks.py`, `scripts/features-reference.py` (golden fixture generator, `fractions` module) — results and fixtures only, not the build | PSF |
| Threads (pthreads via `find_package(Threads)`) | system | `std::jthread` in `faulttwin_concurrency`/`faulttwin_ingest` | system |
| libpq | system package; 16.15 (Ubuntu 24.04 `libpq-dev`) for the Linux runs; Homebrew `libpq` on macOS (19.0 in the owner's run) | `faulttwin_collectors_pg`, `faulttwin-monitor`, integration tests (Phase 3, ADR-0015). Optional: found by `find_package(PostgreSQL)` | PostgreSQL Licence |
| PostgreSQL server image | `postgres:17.12-bookworm@sha256:1eff563f6c868b9b433566e9f3cf6e5c79fbd3b95bf70ec7277d376cdbfc50d7` (multi-arch index) | Lab cluster only (`compose.yaml`, ADR-0014) | PostgreSQL Licence (image contents: Debian packages under their own licences) |
| Docker Engine / Compose | 29.9.3 / v5.7.1 used for the Linux runs; the owner's Mac has Docker 29.6.2 / Compose v5.3.1 | Lab cluster, integration tests | Apache-2.0 |
| Drogon | v1.9.9, commit `38dd5fea31a7a2727c0a6f6b6b04252374796cab` via FetchContent (`SYSTEM`; HTTP core only: no ORM, ctl, examples, Brotli or YAML) | `faulttwind` HTTP server (Phase 5, ADR-0021) | MIT |
| Trantor | Drogon's submodule at `8bf280ba043eb77c3eb02121065e0891a9a4f431` | Drogon's event loop | BSD-3-Clause |
| jsoncpp | system package; 1.9.5 (Ubuntu 24.04 `libjsoncpp-dev`) for the Linux runs; Homebrew `jsoncpp` on macOS | Required by Drogon's headers (FaultTwin's own JSON code is in-tree) | MIT / public domain |
| uuid, zlib | system packages (`uuid-dev`, `zlib1g-dev` on Ubuntu 24.04); macOS system libraries | Required for Drogon. OpenSSL is deliberately not used (`TRANTOR_USE_TLS=none`, ADR-0021) | BSD-3-Clause / zlib |
| Prometheus image | `prom/prometheus:v3.5.0@sha256:63805ebb8d2b3920190daf1cb14a60871b16fd38bed42b857a3182bc621f4996` | Self-observability, `observability` Compose profile only | Apache-2.0 |
| Grafana image | `grafana/grafana:12.1.0@sha256:6ac590e7cabc2fbe8d7b8fc1ce9c9f0582177b334e0df9c927ebd9670469440f` | Self-observability dashboard, `observability` profile only; separate unmodified service | AGPL-3.0 |
| curl | system | `scripts/e2e-faulttwind.sh` only | curl licence |
| actions/checkout | v4.4.0, `11d5960a326750d5838078e36cf38b85af677262` | CI | MIT |
| actions/upload-artifact | v4.6.2, `ea165f8d65b6e75b540449e92b4886f43607fa02` | CI | MIT |

No runtime third-party library is linked into `faulttwin-synth`, `faulttwin_telemetry`,
`faulttwin_concurrency`, `faulttwin_ingest`, `faulttwin_collectors`, `faulttwin_features` or
`faulttwin_detection` (C++ standard library and the system thread library only; Phase 4 added no
dependency, and the k-NN and perceptron detectors are implemented in-tree, ADR-0018). `faulttwin_collectors_pg` and `faulttwin-monitor` link libpq. `faulttwind` links Drogon, Trantor and jsoncpp in addition to libpq; the
Phase 5 libraries `faulttwin_core`, `faulttwin_observability`, `faulttwin_storage` and
`faulttwin_api` use the standard library only (metrics and logging are in-tree, ADR-0019), and
`faulttwin_storage_pg` uses libpq. The meta-db reuses the PostgreSQL 17.12 image digest. libpq is
taken from the system rather than pinned: it is a stable C ABI, and the server version that matters
is pinned in the image. The 16.x client talks to the 17.12 server; libpq is backwards and forwards
compatible for the protocol and functions used.

## Planned for later phases (candidates, not yet resolved or built)

The tables below are a plan. Each phase that introduces a dependency records the exact resolved
version here and justifies it in its phase report.

## Toolchain
| Component | Candidate | Notes |
|---|---|---|
| OS (build/CI) | Ubuntu 24.04 LTS | Dev container + GitHub-hosted runners |
| Compilers | GCC 14, Clang 18 or newer | Both in CI; Clang for TSan |
| CMake | ≥ 3.28 | Presets v6, C++20 modules **not** used |
| Ninja | ≥ 1.11 | |
| vcpkg | not adopted in Phase 5 (ADR-0021: FetchContent with commit pins continues) | Revisit with ONNX Runtime (Phase 7) |
| clang-format / clang-tidy | same major as Clang | Version-pinned to avoid churn |

## C++ libraries
| Library | Candidate | Purpose | Licence (verify) |
|---|---|---|---|
| Drogon | in use since Phase 5 (v1.9.9, see above) | REST API, HTTP client | MIT |
| Boost (Asio, headers) | 1.86+ | RTT probes / timers where justified | BSL-1.0 |
| Protobuf | deferred with gRPC (ADR-0022); the incident log is the meta-db | Telemetry schemas when gRPC arrives | BSD-3-Clause |
| gRPC | deferred in Phase 5 (ADR-0022) | Remote telemetry transport | Apache-2.0 |
| ONNX Runtime | 1.2x (CPU) | Native inference | MIT |
| libpq | in use since Phase 3 (system package) | Collectors, meta-db | PostgreSQL |
| spdlog | not adopted (in-tree logger, ADR-0019) | — | MIT |
| GoogleTest | 1.15+ | Unit/integration tests | BSD-3-Clause |
| Google Benchmark | 1.9.4 (in use since Phase 2) | Benchmarks | Apache-2.0 |

## Infrastructure
| Component | Candidate | Notes |
|---|---|---|
| PostgreSQL | 17.12 (decided in Phase 3, ADR-0014) | Primary + 2 streaming replicas; meta-db since Phase 5 |
| Toxiproxy | 2.x latest | Network faults |
| Prometheus | 3.5.0 in use since Phase 5 (see above) | Self-observability |
| Grafana | 12.1.0 in use since Phase 5 (see above) | Provisioned dashboards |
| Docker Engine / Compose | Compose v2 | Docker Desktop on macOS supported |
| stress-ng | distro package | Resource pressure sidecar |

## Python (offline ML)
| Package | Candidate | Notes |
|---|---|---|
| Python | 3.12 | |
| scikit-learn | 1.6/1.7 line | Must be supported by the pinned skl2onnx |
| skl2onnx | latest compatible | Conversion support verified in Phase 7 |
| onnxruntime (Python) | same minor as C++ ORT | Parity tests |
| numpy / pandas | 2.x | |
| Lock file | `uv.lock` or `requirements.lock` with hashes | Decide in Phase 7 |

## Frontend
| Package | Candidate | Notes |
|---|---|---|
| Node.js | 22 or 24 LTS | |
| React | 19 | |
| TypeScript | 5.x | strict mode |
| Vite | current major | |
| Charts | Apache ECharts | Time series + graph topology in one library |
| Lock file | `package-lock.json` | |

## External services
| Service | Usage | Required? |
|---|---|---|
| OpenAI API | Incident explanations; model name is configuration | Optional (template fallback) |

## Licence obligations
Phase 12 generates `THIRD_PARTY_NOTICES.md` from the resolved dependency set and keeps each
licence text required for redistribution in container images.
