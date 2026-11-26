# Benchmarks. Only meaningful in Release builds (the `release` preset enables
# them); sanitizer and Debug numbers must never be reported (ADR-0011).
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  message(WARNING "FAULTTWIN_BUILD_BENCHMARKS is ON in a ${CMAKE_BUILD_TYPE} build; "
                  "results from this build are not valid measurements.")
endif()

add_executable(faulttwin_bench ingest_bench.cpp)
target_link_libraries(faulttwin_bench PRIVATE faulttwin::ingest benchmark::benchmark)
faulttwin_set_project_warnings(faulttwin_bench)

# Smoke test: the benchmark binary starts, runs the cheapest benchmark once and
# exits 0. It checks that the binary works, not how fast anything is.
if(FAULTTWIN_BUILD_TESTS)
  add_test(NAME bench.smoke
           COMMAND faulttwin_bench --benchmark_filter=BM_BoundedQueuePushPop
                   --benchmark_min_time=1x --benchmark_repetitions=1)
endif()
