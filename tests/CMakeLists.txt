include(GoogleTest)

add_executable(faulttwin_unit_tests
  unit/node_id_test.cpp
  unit/node_role_test.cpp
  unit/validation_test.cpp
  unit/synthetic_test.cpp
  unit/cli_options_test.cpp
  unit/cli_report_test.cpp)
target_link_libraries(faulttwin_unit_tests PRIVATE faulttwin::telemetry faulttwin_cli_lib GTest::gtest_main)
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
