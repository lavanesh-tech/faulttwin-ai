include(FetchContent)

# Google Benchmark is pinned to the commit behind the v1.9.4 release tag, for the
# same reproducibility reason as GoogleTest (ADR-0004, ADR-0013). It is used only
# by bench/ and is never linked into product targets.
# License: Apache-2.0 (https://github.com/google/benchmark/blob/main/LICENSE).
FetchContent_Declare(
  googlebenchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG eddb0241389718a23a42db6af5f0164b6e0139af # v1.9.4
  GIT_SHALLOW FALSE)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googlebenchmark)
