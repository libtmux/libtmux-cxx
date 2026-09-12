include("${PROJECT_SOURCE_DIR}/cmake/CLI11.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/NlohmannJson.cmake")
libtmux_resolve_cli11()
libtmux_resolve_nlohmann_json()

add_library(workspace_cli STATIC src/cli.cpp src/services.cpp)
target_include_directories(workspace_cli PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(workspace_cli PRIVATE libtmux_build_options workspace_builder
  CLI11::CLI11 nlohmann_json::nlohmann_json yaml-cpp::yaml-cpp)
target_compile_definitions(workspace_cli PRIVATE
  "LIBTMUX_WORKSPACE_VERSION=\"${LIBTMUX_VERSION_FULL}\"")
get_target_property(_cli11_include CLI11::CLI11 INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(workspace_cli SYSTEM PRIVATE ${_cli11_include})

add_executable(tmux-workspace src/main.cpp)
target_link_libraries(tmux-workspace PRIVATE libtmux_build_options workspace_cli)
if(LIBTMUX_INSTALL)
  install(TARGETS tmux-workspace RUNTIME DESTINATION bin COMPONENT workspace-cli)
endif()

if(LIBTMUX_BUILD_TESTS AND NOT WIN32)
  add_executable(workspace_cli_test tests/cli_test.cpp)
  target_link_libraries(workspace_cli_test PRIVATE libtmux_build_options workspace_cli
    GTest::gtest_main libtmux::testing nlohmann_json::nlohmann_json)
  add_test(NAME consumer.workspace.cli COMMAND workspace_cli_test
    "--gtest_filter=WorkspaceCli.*")
  add_test(NAME consumer.workspace.cli.real-tmux COMMAND workspace_cli_test
    "--gtest_filter=WorkspaceCliTmux.*")
  set_tests_properties(consumer.workspace.cli PROPERTIES LABELS "consumer;contract" TIMEOUT 5)
  set_tests_properties(consumer.workspace.cli.real-tmux PROPERTIES LABELS "consumer;real-tmux" TIMEOUT 5)
endif()
