add_library(mcp_tools INTERFACE)
target_include_directories(
  mcp_tools INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(mcp_tools INTERFACE libtmux::libtmux)

if(LIBTMUX_BUILD_TESTS)
  add_executable(mcp_tools_test tests/mcp_test.cpp)
  target_link_libraries(
    mcp_tools_test
    PRIVATE GTest::gtest_main libtmux_build_options mcp_tools
            libtmux_test_support)
  target_compile_options(mcp_tools_test PRIVATE -fno-sanitize-recover=undefined)

  add_test(NAME consumer.mcp COMMAND mcp_tools_test)
  set_tests_properties(
    consumer.mcp PROPERTIES LABELS "consumer;real-tmux" TIMEOUT 30)
endif()

if(LIBTMUX_BUILD_MCP_SERVER)
  # Resolved here rather than at the top of the file: the tool surface takes
  # named strings and returns text, so the program that frames it as JSON-RPC
  # is the only thing in this tree that needs a JSON parser.
  include("${PROJECT_SOURCE_DIR}/cmake/NlohmannJson.cmake")
  libtmux_resolve_nlohmann_json()

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
