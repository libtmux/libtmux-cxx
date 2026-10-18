# Runs PROGRAM and requires its standard output to be the contents of EXPECTED.
execute_process(
  COMMAND "${PROGRAM}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE actual
  ERROR_VARIABLE errors)
file(READ "${EXPECTED}" expected)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "${PROGRAM} exited ${status}: ${errors}")
endif()
if(NOT actual STREQUAL expected)
  message(FATAL_ERROR
          "${PROGRAM} printed:\n${actual}but ${EXPECTED} holds:\n${expected}")
endif()
