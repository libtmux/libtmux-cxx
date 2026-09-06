include(FetchContent)

# mcp-swap validates the complete document with toml++ before applying its
# comment-preserving text edits. The dependency is private to that tool.
function(libtmux_resolve_tomlplusplus)
  find_package(tomlplusplus 3.4 CONFIG QUIET)
  if(TARGET tomlplusplus::tomlplusplus)
    return()
  endif()
  if(NOT LIBTMUX_FETCH_DEPS)
    message(FATAL_ERROR
      "tomlplusplus 3.4 is required to build mcp-swap; "
      "set LIBTMUX_FETCH_DEPS=ON to use the pinned fallback")
  endif()
  FetchContent_Declare(
    tomlplusplus
    URL https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz
    URL_HASH
      SHA256=8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155
  )
  FetchContent_MakeAvailable(tomlplusplus)
endfunction()
