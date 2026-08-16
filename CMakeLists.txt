cmake_minimum_required(VERSION 3.25)

# Read before project() so the version travels with the package rather than
# being spelled twice. CONFIGURE_DEPENDS is not available here, so the file is
# added to CMAKE_CONFIGURE_DEPENDS below: without it a warm build tree keeps
# shipping the version it was configured with.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" LIBTMUX_VERSION_TEXT)
string(STRIP "${LIBTMUX_VERSION_TEXT}" LIBTMUX_VERSION_TEXT)
set_property(
  DIRECTORY
  APPEND
  PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

# `project(VERSION)` takes digits and dots and nothing else, so a semantic
# version carrying a prerelease tag cannot be handed to it whole. The file
# stays the one place the version is written; this splits it, and the full
# string is what the library reports and what a human sees.
set(LIBTMUX_VERSION_FULL "${LIBTMUX_VERSION_TEXT}")
if(NOT LIBTMUX_VERSION_TEXT MATCHES "^([0-9]+(\\.[0-9]+)*)")
  message(FATAL_ERROR "VERSION does not begin with a number: ${LIBTMUX_VERSION_TEXT}")
endif()
set(LIBTMUX_VERSION_NUMERIC "${CMAKE_MATCH_1}")

project(
  libtmux_cxx
  VERSION "${LIBTMUX_VERSION_NUMERIC}"
  DESCRIPTION "A typed C++ interface to tmux"
  HOMEPAGE_URL "https://github.com/libtmux/libtmux-cxx"
  LANGUAGES CXX)

option(LIBTMUX_BUILD_TESTS "Build libtmux C++ tests" ${PROJECT_IS_TOP_LEVEL})
# Off by default: a build that would reach the network says so and stops,
# rather than silently downloading. Covers the test frameworks and the
# MCP server's JSON parser; the library itself has no dependencies.
option(LIBTMUX_FETCH_DEPS "Download missing dependencies" OFF)
option(LIBTMUX_ENABLE_SANITIZERS "Enable ASan and UBSan" OFF)
option(LIBTMUX_ENABLE_THREAD_SANITIZER "Enable TSan" OFF)
option(LIBTMUX_ENABLE_CLANG_TIDY "Run clang-tidy" OFF)
option(LIBTMUX_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" OFF)
# Off by default when embedded: a project that adds this one with
# add_subdirectory should not have this package's headers and CMake config
# appear in its own install prefix.
option(LIBTMUX_INSTALL "Generate install rules" ${PROJECT_IS_TOP_LEVEL})
option(LIBTMUX_BUILD_EXAMPLES "Build and run the examples"
       ${PROJECT_IS_TOP_LEVEL})
# Off by default: the fuzzer runtime this toolchain ships is built against a
# different standard library from the one the presets select, so the fuzzers
# get their own configuration rather than constraining every other build.
option(LIBTMUX_BUILD_FUZZERS "Build the parser fuzzers" OFF)
# The MCP server is a program, not part of the library, and it is the one
# thing here that needs a JSON parser. Off, because a top-level build of a
# library should not acquire a dependency for a program the builder did not
# ask for — it followed the tests, which follow being top level, so a plain
# `cmake -S . -B build` wanted nlohmann/json and the documented default said
# otherwise. The presets and CI turn it on explicitly.
option(LIBTMUX_BUILD_MCP_SERVER "Build the MCP server program" OFF)
option(LIBTMUX_ENABLE_COVERAGE "Instrument for coverage" OFF)
set(LIBTMUX_CXX_STANDARD "23" CACHE STRING "C++ package standard: 20 or 23")
set_property(CACHE LIBTMUX_CXX_STANDARD PROPERTY STRINGS 20 23)

if(LIBTMUX_ENABLE_SANITIZERS AND LIBTMUX_ENABLE_THREAD_SANITIZER)
  message(FATAL_ERROR "ASan/UBSan and TSan require separate build trees")
endif()

include(cmake/ProjectOptions.cmake)

# Before the first add_subdirectory, not inside the branches that need it.
# CTest writes a registration file per directory only for directories added
# after testing is enabled, while the parent's file lists every subdirectory
# regardless — so enabling it later leaves the generated tree referencing a
# file that was never written, and anything walking that tree stops there.
if(LIBTMUX_BUILD_TESTS OR LIBTMUX_BUILD_EXAMPLES OR LIBTMUX_BUILD_FUZZERS)
  enable_testing()
endif()

add_subdirectory(src)

if(LIBTMUX_BUILD_TESTS)
  include(cmake/GoogleTest.cmake)
  libtmux_resolve_googletest()
  add_subdirectory(examples/workspace)
  add_subdirectory(tests)
endif()

if(LIBTMUX_BUILD_TESTS OR LIBTMUX_BUILD_MCP_SERVER)
  add_subdirectory(apps/mcp)
endif()

if(LIBTMUX_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(LIBTMUX_BUILD_FUZZERS)
  add_subdirectory(tests/fuzz)
endif()
