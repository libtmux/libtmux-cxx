# Fail when the built archive exports a fault-injection entry point.
#
# Driven by `libtmux.archive.carries_no_test_seams`; see the comment there.
execute_process(
  COMMAND "${NM}" --defined-only "${ARCHIVE}"
  OUTPUT_VARIABLE symbols
  ERROR_VARIABLE failure
  RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "could not read ${ARCHIVE}: ${failure}")
endif()
string(REGEX MATCHALL "[^\n]*_for_test[^\n]*" seams "${symbols}")
if(seams)
  string(REPLACE ";" "\n  " listed "${seams}")
  message(
    FATAL_ERROR
      "${ARCHIVE} exports test-only seams:\n  ${listed}\n"
      "Built with LIBTMUX_ENABLE_FAULT_INJECTION left on?")
endif()
