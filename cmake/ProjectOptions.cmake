include(CheckCXXSourceCompiles)

if(NOT IS_ABSOLUTE "${CMAKE_CXX_COMPILER}"
   OR NOT EXISTS "${CMAKE_CXX_COMPILER}")
  message(FATAL_ERROR "CMAKE_CXX_COMPILER is not a resolved executable")
endif()
if(DEFINED ENV{LIBTMUX_EXPECT_COMPILER_ID}
   AND NOT CMAKE_CXX_COMPILER_ID STREQUAL
       "$ENV{LIBTMUX_EXPECT_COMPILER_ID}")
  message(
    FATAL_ERROR
      "expected C++ compiler ID $ENV{LIBTMUX_EXPECT_COMPILER_ID}, "
      "found ${CMAKE_CXX_COMPILER_ID}. Unset LIBTMUX_EXPECT_COMPILER_ID, or "
      "use a preset that does not pin the toolchain (cxx-gcc).")
endif()
if(DEFINED ENV{LIBTMUX_EXPECT_COMPILER_VERSION}
   AND NOT CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL
       "$ENV{LIBTMUX_EXPECT_COMPILER_VERSION}")
  message(
    FATAL_ERROR
      "expected C++ compiler version $ENV{LIBTMUX_EXPECT_COMPILER_VERSION}, "
      "found ${CMAKE_CXX_COMPILER_VERSION}. Unset "
      "LIBTMUX_EXPECT_COMPILER_VERSION to build with this one.")
endif()

if(DEFINED ENV{LIBTMUX_EXPECT_COMPILER_ID})
  set(_libtmux_required_flags "${CMAKE_REQUIRED_FLAGS}")
  set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -std=c++23")
  check_cxx_source_compiles(
    "#include <expected>
    #if !defined(_LIBCPP_VERSION) || _LIBCPP_VERSION != 180100
    #error \"libc++ 18.1 is required\"
    #endif
    static_assert(__cpp_lib_expected >= 202202L);
    int main() { return 0; }"
    LIBTMUX_HAS_LIBCXX_18_1)
  set(CMAKE_REQUIRED_FLAGS "${_libtmux_required_flags}")

  if(NOT LIBTMUX_HAS_LIBCXX_18_1)
    message(FATAL_ERROR "Clang 18.1.3 with libc++ 18.1 is required")
  endif()
endif()

if(NOT LIBTMUX_CXX_STANDARD STREQUAL "20"
   AND NOT LIBTMUX_CXX_STANDARD STREQUAL "23")
  message(FATAL_ERROR "LIBTMUX_CXX_STANDARD must be 20 or 23")
endif()

add_library(libtmux_build_options INTERFACE)
target_compile_features(
  libtmux_build_options
  INTERFACE cxx_std_${LIBTMUX_CXX_STANDARD})

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(
    libtmux_build_options
    INTERFACE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wsign-conversion)
  if(LIBTMUX_WARNINGS_AS_ERRORS)
    target_compile_options(libtmux_build_options INTERFACE -Werror)
  endif()
endif()

if(LIBTMUX_ENABLE_SANITIZERS)
  target_compile_options(
    libtmux_build_options
    INTERFACE -fsanitize=address,undefined)
  target_link_options(
    libtmux_build_options
    INTERFACE -fsanitize=address,undefined)
elseif(LIBTMUX_ENABLE_THREAD_SANITIZER)
  target_compile_options(libtmux_build_options INTERFACE -fsanitize=thread)
  target_link_options(libtmux_build_options INTERFACE -fsanitize=thread)
endif()

if(LIBTMUX_ENABLE_COVERAGE)
  # Source-based coverage, which counts a header's instantiations rather than
  # dropping them: most of this library's code is in headers.
  target_compile_options(
    libtmux_build_options INTERFACE -fprofile-instr-generate -fcoverage-mapping)
  target_link_options(
    libtmux_build_options INTERFACE -fprofile-instr-generate -fcoverage-mapping)
endif()
