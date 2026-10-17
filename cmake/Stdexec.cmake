include(FetchContent)

function(libtmux_resolve_stdexec)
  if(TARGET STDEXEC::stdexec)
    return()
  endif()
  find_package(stdexec CONFIG QUIET)
  if(TARGET STDEXEC::stdexec)
    return()
  endif()
  find_path(LIBTMUX_STDEXEC_INCLUDE_DIR stdexec/execution.hpp)
  if(NOT LIBTMUX_STDEXEC_INCLUDE_DIR)
    if(NOT LIBTMUX_FETCH_DEPS)
      message(FATAL_ERROR
        "stdexec is required for execution adapter tests; provide its include "
        "directory or set LIBTMUX_FETCH_DEPS=ON for the pinned fallback")
    endif()
    FetchContent_Declare(
      stdexec_headers
      URL https://github.com/NVIDIA/stdexec/archive/34fa94719f97f49a553811423fa1200172f22a0e.tar.gz
      URL_HASH SHA256=4aa212c32c351b1e6cf9e14e5632134c8ae6f5c370654eea309bf80adbe41893
      SOURCE_SUBDIR _libtmux_header_only)
    FetchContent_MakeAvailable(stdexec_headers)
    set(LIBTMUX_STDEXEC_INCLUDE_DIR "${stdexec_headers_SOURCE_DIR}/include")
  endif()
  add_library(libtmux_stdexec_headers INTERFACE)
  target_include_directories(libtmux_stdexec_headers SYSTEM INTERFACE
                             "${LIBTMUX_STDEXEC_INCLUDE_DIR}")
  add_library(STDEXEC::stdexec ALIAS libtmux_stdexec_headers)
endfunction()
