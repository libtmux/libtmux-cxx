add_library(mcp_tools STATIC src/tool_catalog.cpp src/tool_model.cpp)
if(NOT WIN32)
  target_sources(mcp_tools PRIVATE src/wait_for_text.cpp)
endif()
target_include_directories(
  mcp_tools PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(
  mcp_tools PUBLIC libtmux::libtmux PRIVATE libtmux_build_options)

if(LIBTMUX_BUILD_TESTS AND NOT WIN32)
  # `libtmux::testing` directly, not this repository's internal alias: the MCP
  # tool surface is here to be a consumer, and a consumer reaches the fixture
  # through the exported target.
  add_executable(mcp_tools_test tests/mcp_test.cpp)
  target_include_directories(mcp_tools_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(
    mcp_tools_test
    PRIVATE GTest::gtest_main libtmux_build_options mcp_tools
            libtmux::testing)
  target_compile_options(mcp_tools_test PRIVATE -fno-sanitize-recover=undefined)

  add_test(NAME consumer.mcp COMMAND mcp_tools_test)
  set_tests_properties(
    consumer.mcp PROPERTIES LABELS "consumer;real-tmux" TIMEOUT 30)
endif()

# The server as a program, over JSON-RPC, against a real tmux. Needs the JSON
# parser and the binary, so it follows the server rather than the tool surface.
if(LIBTMUX_BUILD_TESTS AND LIBTMUX_BUILD_MCP_SERVER AND NOT WIN32)
  add_executable(mcp_protocol_test tests/protocol_test.cpp)
  target_link_libraries(
    mcp_protocol_test
    PRIVATE GTest::gtest_main libtmux_build_options libtmux::testing
            nlohmann_json::nlohmann_json)
  target_compile_definitions(
    mcp_protocol_test
    PRIVATE "LIBTMUX_MCP_SERVER_PATH=\"$<TARGET_FILE:libtmux_mcp_server>\"")
  target_compile_options(mcp_protocol_test PRIVATE -fno-sanitize-recover=undefined)
  add_dependencies(mcp_protocol_test libtmux_mcp_server)

  add_test(NAME consumer.mcp.protocol COMMAND mcp_protocol_test)
  set_tests_properties(
    consumer.mcp.protocol PROPERTIES LABELS "consumer;real-tmux" TIMEOUT 120)
endif()

if(LIBTMUX_BUILD_MCP_SERVER)
  # Resolved here rather than at the top of the file: the library encodes
  # nothing, so everything that needs a JSON parser is in this directory.
  include("${PROJECT_SOURCE_DIR}/cmake/NlohmannJson.cmake")
  libtmux_resolve_nlohmann_json()

  # The validator is Python because the schema is JSON Schema and a checked
  # implementation of that already exists — `jsonschema` is a dependency of
  # this repository's tooling. Writing a second one in C++ to check the first
  # would be checking the checker.
  find_package(Python3 COMPONENTS Interpreter QUIET)

  if(LIBTMUX_BUILD_TESTS AND NOT WIN32 AND Python3_Interpreter_FOUND)
    # The wire format for a lowered filter expression, and the corpus the
    # published schema is validated against. Two halves that only mean
    # something together: this writes documents a real validator then reads,
    # instead of a test restating in C++ what it hopes the schema says.
    add_executable(filter_json_test tests/filter_json_test.cpp)
    target_link_libraries(
      filter_json_test
      PRIVATE GTest::gtest_main libtmux_build_options mcp_tools
              nlohmann_json::nlohmann_json)
    target_compile_options(filter_json_test PRIVATE -fno-sanitize-recover=undefined)

    set(LIBTMUX_SCHEMA_CORPUS "${CMAKE_CURRENT_BINARY_DIR}/filter-expressions.jsonl")
    add_test(NAME schema.corpus COMMAND filter_json_test)
    set_tests_properties(
      schema.corpus
      PROPERTIES LABELS "schema" TIMEOUT 30 FIXTURES_SETUP schema_corpus
                 ENVIRONMENT "LIBTMUX_SCHEMA_CORPUS=${LIBTMUX_SCHEMA_CORPUS}")

    # A fixture rather than a bare dependency, so running the validator alone
    # still produces the corpus it reads instead of checking a stale one.
    add_test(
      NAME schema.conformance
      COMMAND Python3::Interpreter -m tools.schema
              --schema "${PROJECT_SOURCE_DIR}/schema/filter-expression-v1.schema.json"
              --corpus "${LIBTMUX_SCHEMA_CORPUS}"
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    set_tests_properties(
      schema.conformance
      PROPERTIES LABELS "schema" TIMEOUT 60 FIXTURES_REQUIRED schema_corpus)
  endif()

  # The tool surface as a program: MCP over stdio, which is JSON-RPC. The JSON
  # lives here and nowhere else — the tool surface itself still takes named
  # strings and returns text.
  add_executable(
    libtmux_mcp_server
    src/batch_response.cpp
    src/cli.cpp
    src/dispatcher.cpp
    src/protocol.cpp
    src/protocol_validation.cpp
    src/schema.cpp
    src/server.cpp
    src/stdio_server.cpp)
  set_target_properties(libtmux_mcp_server PROPERTIES OUTPUT_NAME
                                                      libtmux-mcp-server)
  target_include_directories(libtmux_mcp_server PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(
    libtmux_mcp_server
    PRIVATE libtmux_build_options mcp_tools nlohmann_json::nlohmann_json)
  include("${PROJECT_SOURCE_DIR}/cmake/MinGWRuntime.cmake")
  libtmux_copy_mingw_runtime(libtmux_mcp_server)

  if(WIN32 AND LIBTMUX_BUILD_TESTS)
    find_program(
      LIBTMUX_POWERSHELL_EXECUTABLE
      NAMES powershell.exe pwsh.exe pwsh)
    if(LIBTMUX_POWERSHELL_EXECUTABLE)
      add_test(
        NAME windows.mcp.psmux-smoke
        COMMAND
          "${LIBTMUX_POWERSHELL_EXECUTABLE}" -NoLogo -NoProfile -NonInteractive
          -ExecutionPolicy Bypass -File
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/windows_psmux_smoke.ps1" -Server
          "$<TARGET_FILE:libtmux_mcp_server>")
      set_tests_properties(
        windows.mcp.psmux-smoke
        PROPERTIES LABELS "windows;psmux;real-tmux;mcp" TIMEOUT 120)
    elseif(CMAKE_CROSSCOMPILING)
      message(STATUS "PowerShell unavailable; skipping native Windows MCP smoke")
    else()
      message(FATAL_ERROR "PowerShell is required for the native Windows MCP smoke")
    endif()
  endif()

  if(LIBTMUX_INSTALL)
    # Included here rather than relied on from the library's directory: this
    # one can be configured with the library's install rules turned off.
    include(GNUInstallDirs)
    # A program, so it installs on its own rather than joining the library's
    # export set: nothing links against it, and a consuming CMake project has
    # no target to import.
    install(TARGETS libtmux_mcp_server
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  endif()
endif()
