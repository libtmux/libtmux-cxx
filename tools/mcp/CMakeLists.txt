include("${PROJECT_SOURCE_DIR}/cmake/NlohmannJson.cmake")
libtmux_resolve_nlohmann_json()
include("${PROJECT_SOURCE_DIR}/cmake/Tomlplusplus.cmake")
libtmux_resolve_tomlplusplus()

add_library(
  mcp_swap_core STATIC catalog.cpp config.cpp preflight.cpp storage.cpp
                       transaction.cpp)
target_include_directories(mcp_swap_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
  mcp_swap_core
  PRIVATE libtmux_build_options nlohmann_json::nlohmann_json
          tomlplusplus::tomlplusplus)
target_compile_definitions(mcp_swap_core PRIVATE TOML_EXCEPTIONS=0)

add_executable(mcp_swap main.cpp)
set_target_properties(mcp_swap PROPERTIES OUTPUT_NAME mcp-swap)
target_link_libraries(mcp_swap PRIVATE libtmux_build_options mcp_swap_core)

add_executable(mcp_swap_test test_mcp_swap.cpp)
target_link_libraries(
  mcp_swap_test
  PRIVATE GTest::gtest_main libtmux_build_options mcp_swap_core
          nlohmann_json::nlohmann_json)

add_executable(mcp_swap_fake_server test_fake_server.cpp)
target_link_libraries(
  mcp_swap_fake_server
  PRIVATE libtmux_build_options nlohmann_json::nlohmann_json)
target_compile_definitions(
  mcp_swap_test
  PRIVATE "MCP_SWAP_FAKE_SERVER_PATH=\"$<TARGET_FILE:mcp_swap_fake_server>\"")
add_dependencies(mcp_swap_test mcp_swap_fake_server)

add_test(NAME tools.mcp-swap COMMAND mcp_swap_test)
set_tests_properties(
  tools.mcp-swap PROPERTIES LABELS "tools;contract" TIMEOUT 60)
if(LIBTMUX_ENABLE_SANITIZERS
   AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
   AND CMAKE_CXX_FLAGS MATCHES "stdlib=libc")
  # Ubuntu's split libc++/libc++abi flags dependency exception messages as an
  # allocator mismatch. The C++20 sanitizer preset repeats this with libstdc++.
  set_property(
    TEST tools.mcp-swap APPEND PROPERTY ENVIRONMENT
         "ASAN_OPTIONS=alloc_dealloc_mismatch=0")
endif()
