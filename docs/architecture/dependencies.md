# FaultTwin AI — Dependency Matrix

## Actually used (Phases 1–2)

| Dependency | Version / pin | Used for | Licence |
|---|---|---|---|
| CMake | ≥ 3.25 required; 3.28.3 used for the recorded runs | Build | BSD-3-Clause |
| Ninja | 1.11.1 used for the recorded runs | Generator | Apache-2.0 |
| GCC | 13.3.0 (Ubuntu 24.04) used for the recorded runs | Compiler | GPL (toolchain only) |
| Clang / clang-tidy / clang-format | 18.1.3 (Ubuntu 24.04) used for the recorded runs | Compiler, lint, format | Apache-2.0 with LLVM exception |
| GoogleTest | v1.17.0, commit `52eb8108c5bdec04579160ae17225d66034bd723` via FetchContent | Unit tests only | BSD-3-Clause |
| Google Benchmark | v1.9.4, commit `eddb0241389718a23a42db6af5f0164b6e0139af` via FetchContent (Phase 2, ADR-0013) | `bench/` only, Release preset only | Apache-2.0 |
| Python 3 (standard library only) | 3.13 used for the recorded runs | `scripts/summarize-benchmarks.py` (results only, not the build) | PSF |
| Threads (pthreads via `find_package(Threads)`) | system | `std::jthread` in `faulttwin_concurrency`/`faulttwin_ingest` | system |
| actions/checkout | v4.4.0, `11d5960a326750d5838078e36cf38b85af677262` | CI | MIT |
| actions/upload-artifact | v4.6.2, `ea165f8d65b6e75b540449e92b4886f43607fa02` | CI | MIT |

No runtime third-party library is linked into `faulttwin-synth`, `faulttwin_telemetry`,
`faulttwin_concurrency` or `faulttwin_ingest` (C++ standard library and the system thread library
only).

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
| vcpkg | pinned `builtin-baseline` commit, proposed from Phase 5 | ADR-0004 |
| clang-format / clang-tidy | same major as Clang | Version-pinned to avoid churn |

## C++ libraries
| Library | Candidate | Purpose | Licence (verify) |
|---|---|---|---|
| Drogon | 1.9.x | REST API, HTTP client | MIT |
| Boost (Asio, headers) | 1.86+ | RTT probes / timers where justified | BSL-1.0 |
| Protobuf | version bundled at vcpkg baseline | Telemetry and incident log schemas | BSD-3-Clause |
| gRPC | Phase 5 only if justified (ADR-0003) | Remote telemetry transport | Apache-2.0 |
| ONNX Runtime | 1.2x (CPU) | Native inference | MIT |
| libpq | from PostgreSQL 17 | Collectors, meta-db | PostgreSQL |
| spdlog | 1.x | Structured logging backend | MIT |
| GoogleTest | 1.15+ | Unit/integration tests | BSD-3-Clause |
| Google Benchmark | 1.9.4 (in use since Phase 2) | Benchmarks | Apache-2.0 |

## Infrastructure
| Component | Candidate | Notes |
|---|---|---|
| PostgreSQL | 17.x | Primary + 2 streaming replicas + meta-db; 18.x is an option, decide in Phase 3 |
| Toxiproxy | 2.x latest | Network faults |
| Prometheus | 3.x | Self-observability |
| Grafana | 11.x / 12.x | Provisioned dashboards |
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
