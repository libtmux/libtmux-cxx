include(FetchContent)

function(libtmux_resolve_googletest)
  find_package(GTest 1.17 CONFIG QUIET)
  if(TARGET GTest::gtest_main)
    return()
  endif()
  if(NOT LIBTMUX_FETCH_DEPS)
    message(FATAL_ERROR
      "GoogleTest 1.17 is required when LIBTMUX_BUILD_TESTS=ON; "
      "set LIBTMUX_FETCH_DEPS=ON to use the pinned fallback")
  endif()
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
    URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endfunction()
