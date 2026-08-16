# Examples are programs, not snippets: each builds, runs and is checked by the
# same CTest run as everything else.
#
# Configures two ways — added by the parent build, or standalone against an
# install:
#
#   cmake -S examples -B out -DCMAKE_PREFIX_PATH=<install prefix>
#
# Only the second compiles them against what a reader installs, without this
# repository's include paths and warning flags.
#
# `consume` is the exception: it proves the package links, and never contacts
# tmux.
cmake_minimum_required(VERSION 3.25)

if(NOT TARGET libtmux::libtmux)
  project(libtmux_examples LANGUAGES CXX)
  # `testing` too: an example's private tmux server comes from
  # `libtmux::testing`.
  find_package(libtmux REQUIRED COMPONENTS testing)
  enable_testing()
endif()

# Needs a test framework the examples themselves do not, so it follows the
# parent's tests and is off in a standalone build that did not ask.
option(LIBTMUX_EXAMPLES_BUILD_TESTS "Build the examples' own test suite"
       "${LIBTMUX_BUILD_TESTS}")

add_library(libtmux_example_support INTERFACE)
target_include_directories(
  libtmux_example_support INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
  libtmux_example_support INTERFACE libtmux::libtmux libtmux::testing)

# Same sanitizer flags as the fixture they link; see `src/CMakeLists.txt`.
# Absent standalone, where a consumer supplies their own.
if(TARGET libtmux_build_options)
  target_link_libraries(libtmux_example_support INTERFACE libtmux_build_options)
endif()

foreach(example 01-tour 02-workspace 03-filter 04-errors 05-readme 06-streaming)
  string(REPLACE "-" "_" target "libtmux_example_${example}")
  add_executable(${target} "${example}.cpp")
  target_link_libraries(${target} PRIVATE libtmux_example_support)

  add_test(NAME example.${example} COMMAND ${target})
  set_tests_properties(
    example.${example} PROPERTIES LABELS "example;real-tmux" TIMEOUT 60)
endforeach()

# A separate project; see `tests/README.md`.
if(LIBTMUX_EXAMPLES_BUILD_TESTS OR TARGET GTest::gtest_main)
  add_subdirectory(tests)
endif()
