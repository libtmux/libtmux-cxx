include(FetchContent)
include(CheckCXXSourceCompiles)

function(libtmux_resolve_tl_function_ref out_include_dir out_source_dir)
  set(CMAKE_CXX_STANDARD ${LIBTMUX_CXX_STANDARD})
  check_cxx_source_compiles("#include <version>
#if !defined(__cpp_lib_function_ref) || __cpp_lib_function_ref < 202306L
#error native library facility unavailable
#endif
int main() {}" LIBTMUX_HAS_NATIVE_TL_FUNCTION_REF)
  if(LIBTMUX_HAS_NATIVE_TL_FUNCTION_REF)
    set(${out_include_dir} "" PARENT_SCOPE)
    set(${out_source_dir} "" PARENT_SCOPE)
    return()
  endif()
  find_path(LIBTMUX_TL_FUNCTION_REF_INCLUDE_DIR NAMES tl/function_ref.hpp)
  if(LIBTMUX_TL_FUNCTION_REF_INCLUDE_DIR)
    set(${out_include_dir} "${LIBTMUX_TL_FUNCTION_REF_INCLUDE_DIR}" PARENT_SCOPE)
    set(${out_source_dir} "" PARENT_SCOPE)
    return()
  endif()
  if(NOT LIBTMUX_FETCH_DEPS)
    message(FATAL_ERROR
      "tl/function_ref.hpp is required by this standard library; install it or set "
      "LIBTMUX_FETCH_DEPS=ON to use the pinned header-only fallback")
  endif()
  FetchContent_Declare(
    tl_function_ref
    URL https://codeload.github.com/TartanLlama/function_ref/tar.gz/095ed60a37168563e61ca5535e3cec0aba099dfb
    URL_HASH SHA256=6b7ec0c7e82e8f9c93bf43492ff98899771a169d4aaebe7828f2644a590d3c3f
    SOURCE_SUBDIR _libtmux_header_only)
  FetchContent_MakeAvailable(tl_function_ref)
  set(resolved "${tl_function_ref_SOURCE_DIR}/include")
  set(${out_include_dir} "${resolved}" PARENT_SCOPE)
  set(${out_source_dir} "${tl_function_ref_SOURCE_DIR}" PARENT_SCOPE)
endfunction()
