include(FetchContent)

function(libtmux_resolve_cli11)
  find_package(CLI11 2.7 CONFIG QUIET)
  if(TARGET CLI11::CLI11)
    return()
  endif()
  if(NOT LIBTMUX_FETCH_DEPS)
    message(FATAL_ERROR
      "CLI11 2.7 is required for the workspace CLI; "
      "set LIBTMUX_FETCH_DEPS=ON to use the pinned fallback")
  endif()
  FetchContent_Declare(
    cli11
    URL https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.7.2.tar.gz
    URL_HASH SHA256=46eef3101da70852ec7af026e09d485ccee81813331c8c6052d39344443b83da)
  set(CLI11_BUILD_TESTS OFF CACHE INTERNAL "")
  set(CLI11_BUILD_EXAMPLES OFF CACHE INTERNAL "")
  FetchContent_MakeAvailable(cli11)
endfunction()
