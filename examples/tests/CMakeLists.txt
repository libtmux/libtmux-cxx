# The examples' test suite — a consumer of the package, not a part of it.
#
# Everything it uses arrives through `find_package(libtmux COMPONENTS testing)`:
# no path into this repository's build tree, no private headers, no compile
# definition carrying an answer the library's own configure step worked out.
#
# It configures standalone, which is where that fails if it is not true:
#
#   cmake -S examples -B out -DCMAKE_PREFIX_PATH=<prefix>
cmake_minimum_required(VERSION 3.25)

if(NOT TARGET libtmux::testing)
  project(libtmux_example_tests LANGUAGES CXX)
  find_package(libtmux REQUIRED COMPONENTS testing)
  include(CTest)
endif()

if(NOT TARGET GTest::gtest_main)
  # `libtmux::testing` names no framework, so this suite brings its own. It
  # reuses the library's resolver — a system GoogleTest if there is one, the
  # pinned fallback under `LIBTMUX_FETCH_DEPS` — because a build using libc++
  # cannot link a distribution GoogleTest built against libstdc++, and the
  # symptom is a page of `std::__cxx11` link errors.
  include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/GoogleTest.cmake")
  libtmux_resolve_googletest()
endif()

add_executable(libtmux_example_test example_test.cpp)
target_link_libraries(
  libtmux_example_test PRIVATE GTest::gtest_main libtmux::testing)

# Same sanitizer flags as the fixture it links; see `src/CMakeLists.txt`.
if(TARGET libtmux_build_options)
  target_link_libraries(libtmux_example_test PRIVATE libtmux_build_options)
endif()

# The examples are programs this suite runs; a consumer of an installed
# package has no layout convention to guess from.
target_compile_definitions(
  libtmux_example_test
  PRIVATE
    "LIBTMUX_EXAMPLE_BINARY_DIR=\"$<TARGET_FILE_DIR:libtmux_example_01_tour>\"")

find_program(LIBTMUX_EXAMPLE_TEST_TMUX NAMES tmux REQUIRED)
target_compile_definitions(libtmux_example_test PRIVATE
  LIBTMUX_TEST_TMUX_PATH="${LIBTMUX_EXAMPLE_TEST_TMUX}")

add_dependencies(libtmux_example_test libtmux_example_07_default libtmux_example_08_lifecycle)
add_test(NAME example.07-default COMMAND libtmux_example_test --gtest_filter=OrdinaryExample.*)
set_tests_properties(example.07-default PROPERTIES LABELS "example;real-tmux" TIMEOUT 60)

add_test(NAME examples.suite COMMAND libtmux_example_test)
set_tests_properties(
  examples.suite PROPERTIES LABELS "example;consumer;real-tmux" TIMEOUT 300)

add_test(NAME example.08-lifecycle COMMAND libtmux_example_test --gtest_filter=LifecycleExample.*)
set_tests_properties(example.08-lifecycle PROPERTIES LABELS "example;real-tmux" TIMEOUT 60)
