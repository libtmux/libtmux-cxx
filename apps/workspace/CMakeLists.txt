include("${PROJECT_SOURCE_DIR}/cmake/CLI11.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/NlohmannJson.cmake")
libtmux_resolve_cli11()
libtmux_resolve_nlohmann_json()

add_library(workspace_cli STATIC
  src/cli.cpp src/services.cpp src/process.cpp src/progress.cpp src/completion.cpp)
target_include_directories(workspace_cli PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(workspace_cli PRIVATE "${PROJECT_SOURCE_DIR}/src")
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
  target_compile_definitions(workspace_cli_test PRIVATE
    "LIBTMUX_WORKSPACE_BINARY=\"$<TARGET_FILE:tmux-workspace>\"")
  add_dependencies(workspace_cli_test tmux-workspace)
  target_link_libraries(workspace_cli_test PRIVATE libtmux_build_options workspace_cli workspace_builder
    GTest::gtest_main libtmux::testing nlohmann_json::nlohmann_json)
  add_test(NAME consumer.workspace.cli COMMAND workspace_cli_test
    "--gtest_filter=WorkspaceCli.*")
  add_test(NAME consumer.workspace.cli.real-tmux COMMAND workspace_cli_test
    "--gtest_filter=WorkspaceCliTmux.*")
  set_tests_properties(consumer.workspace.cli PROPERTIES LABELS "consumer;contract" TIMEOUT 5)
  set_tests_properties(consumer.workspace.cli.real-tmux PROPERTIES LABELS "consumer;real-tmux" TIMEOUT 60)

  # What the in-process suite cannot reach: a real controlling terminal and the
  # foreground process group around it, and the generated completion scripts
  # executed by the shells they are written for.
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    set(_verify_cli "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_cli.py")
    set(_verify_process "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_process.py")
    add_test(NAME consumer.workspace.verify.cli
      COMMAND Python3::Interpreter "${_verify_cli}"
        --binary "$<TARGET_FILE:tmux-workspace>"
        --iterations 1
        --output "${CMAKE_CURRENT_BINARY_DIR}/verify-cli.json")
    add_test(NAME consumer.workspace.verify.process
      COMMAND Python3::Interpreter "${_verify_process}"
        --binary "$<TARGET_FILE:tmux-workspace>"
        --load
        --output "${CMAKE_CURRENT_BINARY_DIR}/verify-process.json")
    add_test(NAME consumer.workspace.verify.process.logging
      COMMAND Python3::Interpreter "${_verify_process}"
        --binary "$<TARGET_FILE:tmux-workspace>"
        --logging
        --output "${CMAKE_CURRENT_BINARY_DIR}/verify-process-logging.json")
    set_tests_properties(
      consumer.workspace.verify.cli
      consumer.workspace.verify.process
      consumer.workspace.verify.process.logging
      PROPERTIES LABELS "consumer;real-tmux" TIMEOUT 120)

    # A verifier nobody has watched fail is not a gate. Each harness is run
    # again against a program that answers nothing, and passes only on the
    # diagnostic it raises for that.
    set(_unanswering "${CMAKE_CURRENT_BINARY_DIR}/unanswering-workspace")
    file(GENERATE OUTPUT "${_unanswering}" CONTENT "#!/bin/sh\nexit 1\n"
      FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    add_test(NAME consumer.workspace.verify.cli.refuses
      COMMAND Python3::Interpreter "${_verify_cli}"
        --binary "${_unanswering}"
        --iterations 1
        --output "${CMAKE_CURRENT_BINARY_DIR}/verify-cli-refused.json")
    add_test(NAME consumer.workspace.verify.process.refuses
      COMMAND Python3::Interpreter "${_verify_process}"
        --binary "${_unanswering}")
    set_tests_properties(consumer.workspace.verify.cli.refuses PROPERTIES
      LABELS "consumer;real-tmux" TIMEOUT 120
      PASS_REGULAR_EXPRESSION "--generate-completion failed")
    set_tests_properties(consumer.workspace.verify.process.refuses PROPERTIES
      LABELS "consumer;real-tmux" TIMEOUT 120
      PASS_REGULAR_EXPRESSION "AssertionError")
  endif()
endif()
