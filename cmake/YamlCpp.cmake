include(FetchContent)

# YAML belongs to a consumer, not to the library. The workspace builder reads
# tmuxp documents, so it needs a parser; nothing under `src/` does, and the
# installed package neither links nor mentions this.
function(libtmux_resolve_yaml_cpp)
  find_package(yaml-cpp 0.8 CONFIG QUIET)
  if(TARGET yaml-cpp::yaml-cpp)
    return()
  endif()
  if(NOT LIBTMUX_FETCH_DEPS)
    message(FATAL_ERROR
      "yaml-cpp 0.8 is required to build the workspace consumer; "
      "set LIBTMUX_FETCH_DEPS=ON to use the pinned fallback")
  endif()
  FetchContent_Declare(
    yaml_cpp
    URL https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.tar.gz
    URL_HASH
      SHA256=fbe74bbdcee21d656715688706da3c8becfd946d92cd44705cc6098bb23b3a16
  )
  set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(yaml_cpp)
endfunction()
