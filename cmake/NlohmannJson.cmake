include(FetchContent)

# JSON belongs to the MCP consumer, not to the library. The library encodes
# nothing: the tool surface takes named strings and returns text, and turning
# that into a protocol frame is the server's job — which is where this is used.
function(libtmux_resolve_nlohmann_json)
  find_package(nlohmann_json 3.11 CONFIG QUIET)
  if(TARGET nlohmann_json::nlohmann_json)
    return()
  endif()
  if(NOT LIBTMUX_FETCH_DEPS)
    message(FATAL_ERROR
      "nlohmann_json 3.11 is required to build the MCP server; "
      "set LIBTMUX_FETCH_DEPS=ON to use the pinned fallback")
  endif()
  FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH
      SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  )
  set(JSON_BuildTests OFF CACHE INTERNAL "")
  set(JSON_Install OFF CACHE INTERNAL "")
  FetchContent_MakeAvailable(nlohmann_json)
endfunction()
