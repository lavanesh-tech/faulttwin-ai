# Runs `faulttwin-detect evaluate` twice with identical arguments and fails unless every
# output file is byte-identical; then checks that a different test split changes the result.
set(args evaluate --validation-seeds 1-3 --test-seeds 101-102 --shifted-seeds none)

foreach(run a b)
  file(REMOVE_RECURSE "${OUT_DIR}/detect_${run}")
  execute_process(COMMAND "${CLI}" ${args} --out "${OUT_DIR}/detect_${run}"
                  OUTPUT_QUIET RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "faulttwin-detect run ${run} exited with ${rc}")
  endif()
endforeach()

foreach(name summary.json summary.md tuning-tuned.csv tuning-naive.csv tuning-knn.csv
             tuning-perceptron.csv alerts-test.csv)
  file(SHA256 "${OUT_DIR}/detect_a/${name}" hash_a)
  file(SHA256 "${OUT_DIR}/detect_b/${name}" hash_b)
  if(NOT hash_a STREQUAL hash_b)
    message(FATAL_ERROR "${name} differs between identical runs")
  endif()
endforeach()

file(REMOVE_RECURSE "${OUT_DIR}/detect_c")
execute_process(COMMAND "${CLI}" evaluate --validation-seeds 1-3 --test-seeds 103-104
                        --shifted-seeds none --out "${OUT_DIR}/detect_c"
                OUTPUT_QUIET RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "faulttwin-detect run c exited with ${rc}")
endif()
file(SHA256 "${OUT_DIR}/detect_a/alerts-test.csv" hash_a)
file(SHA256 "${OUT_DIR}/detect_c/alerts-test.csv" hash_c)
if(hash_a STREQUAL hash_c)
  message(FATAL_ERROR "different test seeds produced identical alerts")
endif()
message(STATUS "Deterministic detection output verified")
