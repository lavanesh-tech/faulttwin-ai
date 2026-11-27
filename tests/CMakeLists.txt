include(GoogleTest)

add_executable(faulttwin_unit_tests
  unit/node_id_test.cpp
  unit/node_role_test.cpp
  unit/validation_test.cpp
  unit/synthetic_test.cpp
  unit/cli_options_test.cpp
  unit/cli_report_test.cpp
  unit/bounded_queue_test.cpp
  unit/queue_stress_test.cpp
  unit/order_tracker_test.cpp
  unit/pipeline_test.cpp
  unit/collectors/primitives_test.cpp
  unit/collectors/pg_collector_test.cpp
  unit/collectors/rates_topology_test.cpp
  unit/collectors/linux_resources_test.cpp
  unit/features/rolling_window_test.cpp
  unit/features/feature_extractor_test.cpp
  unit/detection/rule_engine_test.cpp
  unit/detection/scenario_evaluation_test.cpp
  unit/detection/detect_options_test.cpp
  unit/detection/online_detector_test.cpp
  unit/detection/learned_test.cpp)
# faulttwin_cli_lib links faulttwin::telemetry PUBLICly, so the telemetry library
# arrives transitively. Listing it again duplicates it on the link line, which
# Apple's linker reports as "ignoring duplicate libraries".
# faulttwin::ingest brings faulttwin::concurrency with it (PUBLIC), and both share
# faulttwin::telemetry with faulttwin_cli_lib; CMake de-duplicates static libraries
# that arrive through different direct dependencies.
# faulttwin_monitor_lib also brings faulttwin::collectors; it is no longer listed directly
# because faulttwin_detect_lib provides it in every configuration.
# faulttwin_detect_lib brings faulttwin::detection, faulttwin::features and
# faulttwin::collectors with it (PUBLIC).
target_link_libraries(faulttwin_unit_tests PRIVATE faulttwin_cli_lib faulttwin::ingest
                                                   faulttwin_detect_lib GTest::gtest_main)
if(FAULTTWIN_HAVE_LIBPQ)
  target_sources(faulttwin_unit_tests PRIVATE unit/monitor_cli_test.cpp)
  target_link_libraries(faulttwin_unit_tests PRIVATE faulttwin_monitor_lib)
endif()
faulttwin_set_project_warnings(faulttwin_unit_tests)

# PRE_TEST discovery lists tests when ctest runs rather than at build time, so a
# sanitizer runtime problem surfaces as a test failure instead of a build error.
gtest_discover_tests(faulttwin_unit_tests DISCOVERY_MODE PRE_TEST)

# ---- CLI tests: run the real executable ---------------------------------------
add_test(NAME cli.help COMMAND faulttwin-synth --help)
set_tests_properties(cli.help PROPERTIES PASS_REGULAR_EXPRESSION "Usage: ")

add_test(NAME cli.rejects_unknown_option COMMAND faulttwin-synth --bogus)
set_tests_properties(cli.rejects_unknown_option PROPERTIES WILL_FAIL TRUE)

add_test(NAME cli.rejects_out_of_range_fraction COMMAND faulttwin-synth --invalid-fraction 1.5)
set_tests_properties(cli.rejects_out_of_range_fraction PROPERTIES WILL_FAIL TRUE)

add_test(NAME cli.ground_truth_matches
         COMMAND faulttwin-synth --nodes 5 --samples-per-node 400 --seed 7
                 --invalid-fraction 0.3 --quiet)
set_tests_properties(cli.ground_truth_matches PROPERTIES PASS_REGULAR_EXPRESSION
                     "ground-truth mismatches   0\n")

add_test(NAME cli.deterministic_output
         COMMAND ${CMAKE_COMMAND}
                 -DCLI=$<TARGET_FILE:faulttwin-synth>
                 -DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/check_deterministic.cmake)

# ---- faulttwin-detect (Phase 4) ---------------------------------------------------------
add_test(NAME cli.detect_help COMMAND faulttwin-detect --help)
set_tests_properties(cli.detect_help PROPERTIES PASS_REGULAR_EXPRESSION "validation scenarios only")

add_test(NAME cli.detect_rejects_overlapping_splits
         COMMAND faulttwin-detect evaluate --validation-seeds 1-10 --test-seeds 5-20)
set_tests_properties(cli.detect_rejects_overlapping_splits PROPERTIES WILL_FAIL TRUE)

add_test(NAME cli.detect_scenario COMMAND faulttwin-detect scenario --seed 3 --format jsonl)
set_tests_properties(cli.detect_scenario PROPERTIES PASS_REGULAR_EXPRESSION
                     "\"type\":\"episode\".*\"type\":\"detection\"")

add_test(NAME cli.detect_deterministic
         COMMAND ${CMAKE_COMMAND}
                 -DCLI=$<TARGET_FILE:faulttwin-detect>
                 -DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/check_detect_deterministic.cmake)

# ---- Integration tests (need libpq; cluster tests also need FT_INTEGRATION=1) ----------
# Tests that need the Docker Compose lab skip unless FT_INTEGRATION=1, so plain `ctest` stays
# green without Docker. They share one cluster and stop/pause its containers, so they hold a
# common resource lock and never run in parallel with each other.
if(FAULTTWIN_HAVE_LIBPQ)
  add_executable(faulttwin_integration_tests integration/lab_test.cpp)
  target_link_libraries(faulttwin_integration_tests PRIVATE faulttwin::collectors_pg
                                                            faulttwin::detection GTest::gtest_main)
  target_compile_definitions(faulttwin_integration_tests
                             PRIVATE FAULTTWIN_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  faulttwin_set_project_warnings(faulttwin_integration_tests)
  gtest_discover_tests(faulttwin_integration_tests DISCOVERY_MODE PRE_TEST
                       TEST_PREFIX "integration."
                       PROPERTIES LABELS integration RESOURCE_LOCK faulttwin_lab TIMEOUT 180)

  add_test(NAME cli.monitor_help COMMAND faulttwin-monitor --help)
  set_tests_properties(cli.monitor_help PROPERTIES PASS_REGULAR_EXPRESSION "FT_MONITOR_NODES")
  add_test(NAME cli.monitor_requires_password COMMAND ${CMAKE_COMMAND} -E env
           --unset=FT_MONITOR_PASSWORD FT_MONITOR_NODES=a=127.0.0.1:1 $<TARGET_FILE:faulttwin-monitor>)
  set_tests_properties(cli.monitor_requires_password PROPERTIES WILL_FAIL TRUE)
endif()
