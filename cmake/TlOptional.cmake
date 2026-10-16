include(FetchContent)
include(CheckCXXSourceCompiles)

function(libtmux_resolve_tl_optional out_include_dir out_source_dir)
  set(CMAKE_CXX_STANDARD ${LIBTMUX_CXX_STANDARD})
  check_cxx_source_compiles("#include <version>
#if !defined(__cpp_lib_optional) || __cpp_lib_optional < 202506L
#error native library facility unavailable
#endif
int main() {}" LIBTMUX_HAS_NATIVE_TL_OPTIONAL)
  if(LIBTMUX_HAS_NATIVE_TL_OPTIONAL)
    set(${out_include_dir} "" PARENT_SCOPE)
    set(${out_source_dir} "" PARENT_SCOPE)
    return()
  endif()
  find_path(LIBTMUX_TL_OPTIONAL_INCLUDE_DIR NAMES tl/optional.hpp)
  if(LIBTMUX_TL_OPTIONAL_INCLUDE_DIR)
    set(${out_include_dir} "${LIBTMUX_TL_OPTIONAL_INCLUDE_DIR}" PARENT_SCOPE)
    set(${out_source_dir} "" PARENT_SCOPE)
    return()
  endif()
  if(NOT LIBTMUX_FETCH_DEPS)
    message(FATAL_ERROR
      "tl/optional.hpp is required by this standard library; install it or set "
      "LIBTMUX_FETCH_DEPS=ON to use the pinned header-only fallback")
  endif()
  FetchContent_Declare(
    tl_optional
    URL https://codeload.github.com/TartanLlama/optional/tar.gz/3a1209de8370bf5fe16362934956144b49591565
    URL_HASH SHA256=456a6cc2351725de53ef88cd5763d82342ae760027f5f20374028e0b1090ae62
    SOURCE_SUBDIR _libtmux_header_only)
  FetchContent_MakeAvailable(tl_optional)
  set(resolved "${tl_optional_SOURCE_DIR}/include")
  set(${out_include_dir} "${resolved}" PARENT_SCOPE)
  set(${out_source_dir} "${tl_optional_SOURCE_DIR}" PARENT_SCOPE)
endfunction()
