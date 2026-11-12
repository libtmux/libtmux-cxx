# Run as `cmake -P`: the tl::expected API check must not reuse a cached answer.
#
# `libtmux_resolve_tl_expected` compiles the copy it finds against the members
# the library needs and caches the answer under LIBTMUX_TL_EXPECTED_HAS_API. A
# build directory outlives the copy it checked: a package upgraded, downgraded
# or swapped for another prefix is a different copy, and the old answer would be
# kept for it. This configures one build directory twice, first against a copy
# that has `and_then` and `transform`, then against one that has not, and the
# second configure must be refused; a cache that was reused would accept it.
#
# Inputs:
#   -DSOURCE_DIR=<repo root>      where cmake/TlExpected.cmake lives
#   -DWORK_DIR=<empty scratch>    the two fake packages, the project, the build
#   -DCXX_COMPILER=<compiler>     compiles the check
#   -DDROP_UNSET=ON               optional: test a copy of the module with its
#                                 `unset(... CACHE)` line removed. That is the
#                                 bug this guards against, so the run must fail;
#                                 it proves the test can.
cmake_minimum_required(VERSION 3.25)

foreach(required SOURCE_DIR WORK_DIR CXX_COMPILER)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "-D${required}= is required")
  endif()
endforeach()
set(RESOLVER "${SOURCE_DIR}/cmake/TlExpected.cmake")

file(REMOVE_RECURSE "${WORK_DIR}")

if(DROP_UNSET)
  file(READ "${RESOLVER}" module)
  string(REPLACE "unset(LIBTMUX_TL_EXPECTED_HAS_API CACHE)" "" mutated "${module}")
  if(mutated STREQUAL module)
    message(FATAL_ERROR "the module no longer has the unset line this mutates")
  endif()
  file(WRITE "${WORK_DIR}/resolver/TlExpected.cmake" "${mutated}")
  set(RESOLVER "${WORK_DIR}/resolver/TlExpected.cmake")
endif()

# Two copies that differ in exactly what the check looks for.
function(fake_package name members)
  set(root "${WORK_DIR}/${name}")
  file(WRITE "${root}/include/tl/expected.hpp"
       "#pragma once\n"
       "namespace tl {\n"
       "template <class T, class E> struct expected {\n"
       "  T v;\n"
       "  expected(T x) : v(x) {}\n"
       "  T value() const { return v; }\n"
       "${members}"
       "};\n"
       "}\n")
  file(WRITE "${root}/tl-expected-config.cmake"
       "add_library(tl::expected INTERFACE IMPORTED)\n"
       "set_target_properties(tl::expected PROPERTIES\n"
       "  INTERFACE_INCLUDE_DIRECTORIES \"${root}/include\")\n"
       "set(tl-expected_FOUND TRUE)\n")
endfunction()

fake_package(with_api
  "  template <class F> auto and_then(F f) const { return f(v); }\n  template <class F> auto transform(F f) const { return expected<decltype(f(v)), E>{f(v)}; }\n")
fake_package(without_api "")

file(WRITE "${WORK_DIR}/project/CMakeLists.txt"
     "cmake_minimum_required(VERSION 3.25)\n"
     "project(tl_stale CXX)\n"
     "include(\"${RESOLVER}\")\n"
     "set(LIBTMUX_FETCH_DEPS OFF)\n"
     "libtmux_resolve_tl_expected(found include_dir install_dir)\n"
     "message(STATUS \"RESULT found=\${found}\")\n")

function(configure package out_status out_text)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${WORK_DIR}/project" -B "${WORK_DIR}/build"
            "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}"
            "-Dtl-expected_DIR=${WORK_DIR}/${package}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE text
    ERROR_VARIABLE text_err)
  set(${out_status} "${status}" PARENT_SCOPE)
  set(${out_text} "${text}${text_err}" PARENT_SCOPE)
endfunction()

configure(with_api first_status first_text)
if(NOT first_status EQUAL 0 OR NOT first_text MATCHES "RESULT found=TRUE")
  message(FATAL_ERROR
    "a copy with and_then and transform was not accepted (status ${first_status}):\n"
    "${first_text}")
endif()

# The same build directory, now pointed at a copy without them.
configure(without_api second_status second_text)
if(second_status EQUAL 0 OR second_text MATCHES "RESULT found=TRUE")
  message(FATAL_ERROR
    "STALE_CACHE_REUSED: the second configure accepted a copy without and_then and transform: the "
    "check reused the answer cached for the first copy:\n${second_text}")
endif()
if(NOT second_text MATCHES "tl::expected 1.1.0 or newer is required")
  message(FATAL_ERROR
    "the second configure failed, but not with the refusal this test expects:\n"
    "${second_text}")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
message(STATUS "tl::expected API check is recomputed for each copy")
