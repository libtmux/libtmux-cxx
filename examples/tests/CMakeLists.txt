# The examples' test suite — a consumer of the package, not a part of it.
#
# Everything it uses arrives through `find_package(libtmux COMPONENTS testing)`.
# It has no path into this repository's build tree, no private headers, and no
# compile definition carrying an answer the library's own configure step
# already worked out. That is the point: if this suite can test tmux code with
# only the installed package, so can anybody.
#
# It configures standalone, which is how CI proves the claim:
#
#   cmake -S examples -B out -DCMAKE_PREFIX_PATH=<prefix>
cmake_minimum_required(VERSION 3.25)

if(NOT TARGET libtmux::testing)
  project(libtmux_example_tests LANGUAGES CXX)
  find_package(libtmux REQUIRED COMPONENTS testing)
  include(CTest)
endif()

if(NOT TARGET GTest::gtest_main)
  # A test framework is the one thing this suite needs that the package does
  # not supply, and deliberately so: `libtmux::testing` names no framework, so
  # picking one is the consumer's business. This reuses the library's own
  # resolver — a system GoogleTest if there is one, the pinned fallback when
  # `LIBTMUX_FETCH_DEPS` is on — because a consumer building with libc++ cannot
  # link a distribution GoogleTest built against libstdc++, and discovering
  # that through a page of `std::__cxx11` link errors is a bad afternoon.
  include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/GoogleTest.cmake")
  libtmux_resolve_googletest()
endif()

add_executable(libtmux_example_test example_test.cpp)
target_link_libraries(
  libtmux_example_test PRIVATE GTest::gtest_main libtmux::testing)
target_compile_features(libtmux_example_test PRIVATE cxx_std_23)

# The examples are programs this suite runs, so it has to be told where they
# landed. A generator expression, not a guessed path: an installed-package
# consumer has no layout to rely on, and this is what one would write.
target_compile_definitions(
  libtmux_example_test
  PRIVATE
    "LIBTMUX_EXAMPLE_BINARY_DIR=\"$<TARGET_FILE_DIR:libtmux_example_01_tour>\"")

add_test(NAME examples.suite COMMAND libtmux_example_test)
set_tests_properties(
  examples.suite PROPERTIES LABELS "example;consumer;real-tmux" TIMEOUT 300)
