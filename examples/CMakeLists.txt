# Examples are programs, not snippets: each one builds, runs, and is checked by
# the same CTest run as everything else, so none of them can quietly rot.
#
# `consume` is the exception and stays one: it exists to prove the installed
# package links, and deliberately never contacts tmux.

add_library(libtmux_example_support INTERFACE)
target_include_directories(
  libtmux_example_support INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(libtmux_example_support INTERFACE libtmux::libtmux)

foreach(example 01-tour 02-workspace 03-filter 04-errors)
  string(REPLACE "-" "_" target "libtmux_example_${example}")
  add_executable(${target} "${example}.cpp")
  target_link_libraries(${target} PRIVATE libtmux_example_support)

  add_test(NAME example.${example} COMMAND ${target})
  set_tests_properties(
    example.${example} PROPERTIES LABELS "example;real-tmux" TIMEOUT 60)
endforeach()
