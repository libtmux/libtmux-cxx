# The driver's hooks do not prove that its parser was instrumented.
set(parser_found FALSE)
foreach(object IN LISTS OBJECTS)
  execute_process(
    COMMAND "${NM}" -C "${object}"
    OUTPUT_VARIABLE symbols
    ERROR_VARIABLE failure
    RESULT_VARIABLE status)
  if(NOT status STREQUAL "0")
    message(FATAL_ERROR "could not inspect ${object}: ${failure}")
  endif()
  if(NOT symbols MATCHES "[ \t]T[ \t]+libtmux::[^\n]*Parser::feed\\(")
    continue()
  endif()
  set(parser_found TRUE)
  foreach(family __sanitizer_cov_ __asan_ __ubsan_)
    if(NOT symbols MATCHES "(^|\n)[ \t]*U[ \t]+_?${family}")
      message(
        FATAL_ERROR
          "${object} defines Parser::feed without ${family} instrumentation")
    endif()
  endforeach()
endforeach()
if(NOT parser_found)
  message(FATAL_ERROR "the control fuzzer does not compile Parser::feed")
endif()
