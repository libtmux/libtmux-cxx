include(FetchContent)
include(CheckCXXSourceCompiles)

function(libtmux_resolve_value_types out_include_dir)
  set(CMAKE_CXX_STANDARD ${LIBTMUX_CXX_STANDARD})
  check_cxx_source_compiles("#include <version>
#if !defined(__cpp_lib_indirect) || __cpp_lib_indirect < 202502L
#error native library facility unavailable
#endif
int main() {}" LIBTMUX_HAS_NATIVE_VALUE_TYPES)
  if(LIBTMUX_HAS_NATIVE_VALUE_TYPES)
    set(${out_include_dir} "" PARENT_SCOPE)
    return()
  endif()
  find_path(LIBTMUX_VALUE_TYPES_INCLUDE_DIR NAMES value_types/indirect.h)
  if(LIBTMUX_VALUE_TYPES_INCLUDE_DIR)
    set(${out_include_dir} "${LIBTMUX_VALUE_TYPES_INCLUDE_DIR}" PARENT_SCOPE)
    return()
  endif()
  if(NOT LIBTMUX_FETCH_DEPS)
    message(FATAL_ERROR
      "value_types/indirect.h is required by this standard library; install it or set "
      "LIBTMUX_FETCH_DEPS=ON to use the pinned header-only fallback")
  endif()
  FetchContent_Declare(
    value_types
    URL https://codeload.github.com/jbcoe/value_types/tar.gz/57f2e7a81aea38fd9cf29b4c0cd1f45c9ce2d415
    URL_HASH SHA256=5dbd41aafa2d05d50c5d598d6dbf25c33e8a2ad882e3e0e4bf527bdd2336c7bc
    SOURCE_SUBDIR _libtmux_header_only)
  FetchContent_MakeAvailable(value_types)
  set(resolved "${CMAKE_CURRENT_BINARY_DIR}/value_types_include")
  file(MAKE_DIRECTORY "${resolved}/value_types")
  foreach(header indirect.h feature_check.h)
    configure_file("${value_types_SOURCE_DIR}/${header}"
                   "${resolved}/value_types/${header}" COPYONLY)
  endforeach()
  set(${out_include_dir} "${resolved}" PARENT_SCOPE)
endfunction()
