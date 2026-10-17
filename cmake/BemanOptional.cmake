include(FetchContent)
include(CheckCXXSourceCompiles)

function(libtmux_resolve_beman_optional out_include_dir)
  set(CMAKE_CXX_STANDARD ${LIBTMUX_CXX_STANDARD})
  check_cxx_source_compiles("#include <version>
#if !defined(__cpp_lib_optional) || __cpp_lib_optional < 202506L
#error native library facility unavailable
#endif
int main() {}" LIBTMUX_HAS_NATIVE_BEMAN_OPTIONAL)
  if(LIBTMUX_HAS_NATIVE_BEMAN_OPTIONAL)
    set(${out_include_dir} "" PARENT_SCOPE)
    return()
  endif()
  find_path(LIBTMUX_BEMAN_OPTIONAL_INCLUDE_DIR NAMES beman/optional/optional.hpp)
  if(LIBTMUX_BEMAN_OPTIONAL_INCLUDE_DIR)
    set(${out_include_dir} "${LIBTMUX_BEMAN_OPTIONAL_INCLUDE_DIR}" PARENT_SCOPE)
    return()
  endif()
  if(NOT LIBTMUX_FETCH_DEPS)
    message(FATAL_ERROR
      "beman/optional/optional.hpp is required by this standard library; install it or set "
      "LIBTMUX_FETCH_DEPS=ON to use the pinned header-only fallback")
  endif()
  FetchContent_Declare(
    beman_optional
    URL https://codeload.github.com/bemanproject/optional/tar.gz/a33449cb8baf312bfeb2aef7e0cd34de66e4eeaf
    URL_HASH SHA256=ecbfe03d15224363af1e5d7728f3d581e129c24af03fa2b79968dff66f288de5
    SOURCE_SUBDIR _libtmux_header_only)
  FetchContent_MakeAvailable(beman_optional)
  set(resolved "${beman_optional_SOURCE_DIR}/include")
  set(${out_include_dir} "${resolved}" PARENT_SCOPE)
endfunction()
