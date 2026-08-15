# Four parsers read input the library does not choose: what tmux printed, what
# a program inside a pane put in a title, what a distribution patched into a
# version string, and whatever arrives on a control socket. None of them is
# reachable through a compile-time contract, so they are fuzzed.
#
# Their own build, because -fsanitize=fuzzer does not compose with the thread
# sanitizer and the runtime this toolchain ships is built against the other
# standard library.

foreach(parser row option version control)
  add_executable(libtmux_fuzz_${parser} ${parser}_fuzzer.cpp)
  target_link_libraries(libtmux_fuzz_${parser} PRIVATE libtmux::libtmux)
  target_compile_options(
    libtmux_fuzz_${parser} PRIVATE -fsanitize=fuzzer,address,undefined -g)
  target_link_options(
    libtmux_fuzz_${parser} PRIVATE -fsanitize=fuzzer,address,undefined)

  # Bounded: the point in CI is that the checked-in corpus still parses and
  # that a short run finds nothing, not to search for hours.
  # libFuzzer writes anything new it finds into the first directory it is
  # given, so that is a scratch one inside the build tree. The checked-in
  # corpus follows it and stays read-only: a run must not rewrite the seeds it
  # was handed.
  set(scratch "${CMAKE_CURRENT_BINARY_DIR}/found/${parser}")
  file(MAKE_DIRECTORY "${scratch}")

  add_test(
    NAME fuzz.${parser}
    COMMAND
      libtmux_fuzz_${parser} "${scratch}"
      "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${parser}s" -runs=20000
      -max_total_time=20 -max_len=4096 -print_final_stats=1)
  set_tests_properties(fuzz.${parser} PROPERTIES LABELS "fuzz" TIMEOUT 120)
endforeach()
