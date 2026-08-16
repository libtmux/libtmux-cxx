add_library(mcp_tools INTERFACE)
target_include_directories(
  mcp_tools INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(mcp_tools INTERFACE libtmux::libtmux)

if(LIBTMUX_BUILD_TESTS)
  # `libtmux::testing` directly, not this repository's internal alias: the MCP
  # tool surface is here to be a consumer, and a consumer reaches the fixture
  # through the exported target.
  add_executable(mcp_tools_test tests/mcp_test.cpp)
  target_link_libraries(
    mcp_tools_test
    PRIVATE GTest::gtest_main libtmux_build_options mcp_tools
            libtmux::testing)
  target_compile_options(mcp_tools_test PRIVATE -fno-sanitize-recover=undefined)

  add_test(NAME consumer.mcp COMMAND mcp_tools_test)
  set_tests_properties(
    consumer.mcp PROPERTIES LABELS "consumer;real-tmux" TIMEOUT 30)
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

  if(LIBTMUX_BUILD_TESTS AND Python3_Interpreter_FOUND)
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
  add_executable(libtmux_mcp_server src/server.cpp)
  set_target_properties(libtmux_mcp_server PROPERTIES OUTPUT_NAME
                                                      libtmux-mcp-server)
  target_link_libraries(
    libtmux_mcp_server
    PRIVATE libtmux_build_options mcp_tools nlohmann_json::nlohmann_json)

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
