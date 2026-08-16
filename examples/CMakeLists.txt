# Examples are programs, not snippets: each one builds, runs, and is checked by
# the same CTest run as everything else, so none of them can quietly rot.
#
# This directory configures two ways. Added by the parent build it uses the
# targets already defined; pointed at on its own — `cmake -S examples -B out
# -DCMAKE_PREFIX_PATH=<install prefix>` — it finds the installed package
# instead. The second way is not a convenience. It is the only thing that
# proves an example compiles against what a reader would actually install,
# rather than against a build tree with this repository's include paths and
# warning flags leaking into it.
#
# `consume` is the exception and stays one: it exists to prove the installed
# package links, and deliberately never contacts tmux.
cmake_minimum_required(VERSION 3.25)

if(NOT TARGET libtmux::libtmux)
  project(libtmux_examples LANGUAGES CXX)
  # The `testing` component too: an example's private tmux server comes from
  # `libtmux::testing`, the same fixture the library's own suite runs on.
  find_package(libtmux REQUIRED COMPONENTS testing)
  include(CTest)
endif()

add_library(libtmux_example_support INTERFACE)
target_include_directories(
  libtmux_example_support INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
  libtmux_example_support INTERFACE libtmux::libtmux libtmux::testing)

foreach(example 01-tour 02-workspace 03-filter 04-errors 05-readme)
  string(REPLACE "-" "_" target "libtmux_example_${example}")
  add_executable(${target} "${example}.cpp")
  target_link_libraries(${target} PRIVATE libtmux_example_support)

  add_test(NAME example.${example} COMMAND ${target})
  set_tests_properties(
    example.${example} PROPERTIES LABELS "example;real-tmux" TIMEOUT 60)
endforeach()
