# Runs the CLI twice with identical arguments and fails unless both outputs are
# byte-identical, then once with a different seed and fails if output matches.
set(args --nodes 4 --samples-per-node 50 --seed 1234 --invalid-fraction 0.25 --format jsonl)

foreach(run a b)
  execute_process(COMMAND "${CLI}" ${args} OUTPUT_FILE "${OUT_DIR}/determinism_${run}.jsonl"
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "CLI run ${run} exited with ${rc}")
  endif()
endforeach()

execute_process(COMMAND "${CLI}" --nodes 4 --samples-per-node 50 --seed 1235
                        --invalid-fraction 0.25 --format jsonl
                OUTPUT_FILE "${OUT_DIR}/determinism_c.jsonl" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "CLI run c exited with ${rc}")
endif()

file(SHA256 "${OUT_DIR}/determinism_a.jsonl" hash_a)
file(SHA256 "${OUT_DIR}/determinism_b.jsonl" hash_b)
file(SHA256 "${OUT_DIR}/determinism_c.jsonl" hash_c)
if(NOT hash_a STREQUAL hash_b)
  message(FATAL_ERROR "Same seed produced different output: ${hash_a} vs ${hash_b}")
endif()
if(hash_a STREQUAL hash_c)
  message(FATAL_ERROR "Different seeds produced identical output: ${hash_a}")
endif()
message(STATUS "Deterministic output verified, sha256=${hash_a}")
