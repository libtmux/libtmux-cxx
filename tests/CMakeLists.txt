add_executable(build_smoke build_smoke_test.cpp)
target_link_libraries(
  build_smoke PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux)
add_test(NAME build_smoke COMMAND build_smoke)
set_tests_properties(build_smoke PROPERTIES LABELS "contract")

# What is left of this suite's own scaffolding once the reusable part has been
# extracted: `libtmux::testing` carries the fixture, and this carries the
# handful of headers that are only meaningful inside this repository —
# descriptor accounting, `/proc` platform guards, the differential wire codec.
#
# An INTERFACE target now, not a static library. It compiles nothing; it says
# "the exported fixture, plus this directory on the include path", which is
# exactly what every test here needs and nothing a consumer would want.
add_library(libtmux_test_support INTERFACE)
target_include_directories(
  libtmux_test_support INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
  libtmux_test_support INTERFACE libtmux::testing GTest::gtest)

add_executable(fake_tmux support/fake_tmux.cpp)
target_link_libraries(fake_tmux PRIVATE libtmux_build_options)

add_executable(
  scoped_tmux_server_test
  scoped_tmux_server_test.cpp
  scoped_tmux_server_failure_test.cpp
  support/process_test.cpp)
target_compile_definitions(
  scoped_tmux_server_test
  PRIVATE LIBTMUX_FAKE_TMUX_PATH="$<TARGET_FILE:fake_tmux>")
# The fixture's own tests reach past its public header into the process
# supervision it is built on — the pidfd reaping, the interposed syscalls
# below. That is a privilege of living in this repository, not part of what
# `libtmux::testing` promises anyone else.
target_include_directories(
  scoped_tmux_server_test PRIVATE "${PROJECT_SOURCE_DIR}/src/testing")
target_link_libraries(
  scoped_tmux_server_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux_test_support)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(
    scoped_tmux_server_test
    PRIVATE
      "-Wl,--wrap=read"
      "-Wl,--wrap=syscall"
      "-Wl,--wrap=waitpid")
endif()
add_dependencies(scoped_tmux_server_test fake_tmux)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  set_property(
    SOURCE
      scoped_tmux_server_test.cpp
      scoped_tmux_server_failure_test.cpp
      support/process_test.cpp
    APPEND
    PROPERTY COMPILE_OPTIONS -Wno-missing-field-initializers)
endif()

# Discover at test time rather than build time. `gtest_discover_tests` defaults
# to running each test binary during the build to enumerate its cases, which
# makes the build depend on those binaries being runnable right then — and on a
# loaded runner one of them produced an empty listing and failed the build with
# a JSON parse error. Nothing is gained by asking during the build; ctest can
# ask when it is about to run them.
set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)

include(GoogleTest)
gtest_discover_tests(
  scoped_tmux_server_test
  TEST_PREFIX "scoped_tmux_server_"
  PROPERTIES LABELS real-tmux TIMEOUT 30)

# CMake 3.28 flattens list-valued gtest_discover_tests properties. Apply the
# complete label list through its documented post-discovery include hook.
set(scoped_tmux_server_properties
    "${CMAKE_CURRENT_BINARY_DIR}/scoped_tmux_server_properties.cmake")
file(
  GENERATE
  OUTPUT "${scoped_tmux_server_properties}"
  CONTENT [=[
foreach(test_name IN LISTS scoped_tmux_server_test_TESTS)
  set_tests_properties(
    "${test_name}" PROPERTIES LABELS "contract;real-tmux" TIMEOUT 30)
endforeach()
]=])
set_property(
  DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
  "${scoped_tmux_server_properties}")

add_subdirectory(differential)

add_executable(libtmux_chain_test chain_test.cpp)
target_link_libraries(
  libtmux_chain_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux
          libtmux_test_support)
target_compile_options(libtmux_chain_test PRIVATE -fno-sanitize-recover=undefined)

gtest_discover_tests(
  libtmux_chain_test
  TEST_PREFIX libtmux.chain.
  PROPERTIES LABELS real-tmux TIMEOUT 30)

add_executable(libtmux_compatibility_test compatibility_test.cpp)
target_compile_definitions(
  libtmux_compatibility_test
  PRIVATE
    LIBTMUX_FORMAT_TOKENS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/data/format-tokens-3.2a.txt"
    LIBTMUX_KEY_NAMES_PATH="${CMAKE_CURRENT_SOURCE_DIR}/data/key-names-3.2a.txt")
target_link_libraries(
  libtmux_compatibility_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux)

add_test(NAME libtmux.compatibility COMMAND libtmux_compatibility_test)
set_tests_properties(libtmux.compatibility PROPERTIES LABELS "libtmux" TIMEOUT 30)

add_executable(libtmux_options_test options_test.cpp)
target_link_libraries(
  libtmux_options_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux
          libtmux_test_support)
target_compile_options(libtmux_options_test PRIVATE -fno-sanitize-recover=undefined)

gtest_discover_tests(
  libtmux_options_test
  TEST_PREFIX libtmux.options.
  PROPERTIES LABELS real-tmux TIMEOUT 60)

# Behavior evidence for the parity ledger lives under integration/, which is
# where the contract's evidence rules require a behavior artifact to be.
add_executable(libtmux_session_navigation_test integration/session_navigation_test.cpp)
target_link_libraries(
  libtmux_session_navigation_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux
          libtmux_test_support)
target_compile_options(
  libtmux_session_navigation_test PRIVATE -fno-sanitize-recover=undefined)

# Registered per case: the parity ledger binds one behavior evidence record
# to one CTest test, so a capability that is classified needs a test of its
# own to point at.
gtest_discover_tests(
  libtmux_session_navigation_test
  TEST_PREFIX libtmux.session.navigation.
  # One label, not a list. gtest_discover_tests flattens a semicolon list
  # before writing its discovery file, so "libtmux;real-tmux" arrives as two
  # arguments and everything after the first becomes a stray property — the
  # test then answers to neither name. Escaping and bracket arguments do not
  # survive generation either.
  PROPERTIES LABELS real-tmux TIMEOUT 60)

add_executable(libtmux_server_options_test integration/server_options_test.cpp)
target_link_libraries(
  libtmux_server_options_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux
          libtmux_test_support)
target_compile_options(
  libtmux_server_options_test PRIVATE -fno-sanitize-recover=undefined)

gtest_discover_tests(
  libtmux_server_options_test
  TEST_PREFIX libtmux.server.options.
  PROPERTIES LABELS real-tmux TIMEOUT 60)

add_executable(libtmux_format_target_test integration/format_and_target_test.cpp)
target_link_libraries(
  libtmux_format_target_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux
          libtmux_test_support)
target_compile_options(
  libtmux_format_target_test PRIVATE -fno-sanitize-recover=undefined)

gtest_discover_tests(
  libtmux_format_target_test
  TEST_PREFIX libtmux.format.target.
  PROPERTIES LABELS real-tmux TIMEOUT 60)

add_executable(libtmux_window_linking_test integration/window_linking_test.cpp)
target_link_libraries(
  libtmux_window_linking_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux
          libtmux_test_support)
target_compile_options(
  libtmux_window_linking_test PRIVATE -fno-sanitize-recover=undefined)

gtest_discover_tests(
  libtmux_window_linking_test
  TEST_PREFIX libtmux.window.linking.
  PROPERTIES LABELS real-tmux TIMEOUT 60)

add_executable(libtmux_snapshot_framing_test integration/snapshot_framing_test.cpp)
target_link_libraries(
  libtmux_snapshot_framing_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux
          libtmux_test_support)
target_compile_options(
  libtmux_snapshot_framing_test PRIVATE -fno-sanitize-recover=undefined)

gtest_discover_tests(
  libtmux_snapshot_framing_test
  TEST_PREFIX libtmux.snapshot.framing.
  PROPERTIES LABELS real-tmux TIMEOUT 60)

add_executable(libtmux_server_identity_test integration/server_identity_test.cpp)
target_link_libraries(
  libtmux_server_identity_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux
          libtmux_test_support)
target_compile_options(
  libtmux_server_identity_test PRIVATE -fno-sanitize-recover=undefined)

gtest_discover_tests(
  libtmux_server_identity_test
  TEST_PREFIX libtmux.server.identity.
  PROPERTIES LABELS real-tmux TIMEOUT 60)

add_executable(libtmux_pane_topology_test integration/pane_topology_test.cpp)
target_link_libraries(
  libtmux_pane_topology_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux
          libtmux_test_support)
target_compile_options(
  libtmux_pane_topology_test PRIVATE -fno-sanitize-recover=undefined)

gtest_discover_tests(
  libtmux_pane_topology_test
  TEST_PREFIX libtmux.pane.topology.
  PROPERTIES LABELS real-tmux TIMEOUT 60)

add_executable(libtmux_wait_for_test integration/wait_for_test.cpp)
target_link_libraries(
  libtmux_wait_for_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux
          libtmux_test_support)
target_compile_options(
  libtmux_wait_for_test PRIVATE -fno-sanitize-recover=undefined)

gtest_discover_tests(
  libtmux_wait_for_test
  TEST_PREFIX libtmux.wait_for.
  PROPERTIES LABELS real-tmux TIMEOUT 90)

add_executable(libtmux_command_test integration/command_test.cpp)
target_link_libraries(
  libtmux_command_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux
          libtmux_test_support)
target_compile_options(
  libtmux_command_test PRIVATE -fno-sanitize-recover=undefined)

gtest_discover_tests(
  libtmux_command_test
  TEST_PREFIX libtmux.command.
  PROPERTIES LABELS real-tmux TIMEOUT 60)

add_executable(libtmux_buffer_test integration/buffer_test.cpp)
target_link_libraries(
  libtmux_buffer_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux
          libtmux_test_support)
target_compile_options(
  libtmux_buffer_test PRIVATE -fno-sanitize-recover=undefined)

gtest_discover_tests(
  libtmux_buffer_test
  TEST_PREFIX libtmux.buffer.
  PROPERTIES LABELS real-tmux TIMEOUT 60)

add_executable(libtmux_environment_test integration/environment_test.cpp)
target_link_libraries(
  libtmux_environment_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux
          libtmux_test_support)
target_compile_options(
  libtmux_environment_test PRIVATE -fno-sanitize-recover=undefined)

gtest_discover_tests(
  libtmux_environment_test
  TEST_PREFIX libtmux.environment.
  PROPERTIES LABELS real-tmux TIMEOUT 60)

add_executable(libtmux_key_binding_test integration/key_binding_test.cpp)
target_link_libraries(
  libtmux_key_binding_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux
          libtmux_test_support)
target_compile_options(
  libtmux_key_binding_test PRIVATE -fno-sanitize-recover=undefined)

gtest_discover_tests(
  libtmux_key_binding_test
  TEST_PREFIX libtmux.keys.
  PROPERTIES LABELS real-tmux TIMEOUT 60)

add_executable(libtmux_scripting_test integration/scripting_test.cpp)
target_link_libraries(
  libtmux_scripting_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux
          libtmux_test_support)
target_compile_options(
  libtmux_scripting_test PRIVATE -fno-sanitize-recover=undefined)

gtest_discover_tests(
  libtmux_scripting_test
  TEST_PREFIX libtmux.scripting.
  PROPERTIES LABELS real-tmux TIMEOUT 60)

add_executable(libtmux_expand_test integration/expand_test.cpp)
target_link_libraries(
  libtmux_expand_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux
          libtmux_test_support)
target_compile_options(
  libtmux_expand_test PRIVATE -fno-sanitize-recover=undefined)

gtest_discover_tests(
  libtmux_expand_test
  TEST_PREFIX libtmux.expand.
  PROPERTIES LABELS real-tmux TIMEOUT 60)

add_executable(libtmux_pane_io_test integration/pane_io_test.cpp)
target_link_libraries(
  libtmux_pane_io_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux
          libtmux_test_support)
target_compile_options(
  libtmux_pane_io_test PRIVATE -fno-sanitize-recover=undefined)

gtest_discover_tests(
  libtmux_pane_io_test
  TEST_PREFIX libtmux.pane.io.
  PROPERTIES LABELS real-tmux TIMEOUT 90)

add_executable(libtmux_window_layout_test integration/window_layout_test.cpp)
target_link_libraries(
  libtmux_window_layout_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux
          libtmux_test_support)
target_compile_options(
  libtmux_window_layout_test PRIVATE -fno-sanitize-recover=undefined)

gtest_discover_tests(
  libtmux_window_layout_test
  TEST_PREFIX libtmux.window.layout.
  PROPERTIES LABELS real-tmux TIMEOUT 60)

add_executable(libtmux_server_sessions_test integration/server_sessions_test.cpp)
target_link_libraries(
  libtmux_server_sessions_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux
          libtmux_test_support)
target_compile_options(
  libtmux_server_sessions_test PRIVATE -fno-sanitize-recover=undefined)

gtest_discover_tests(
  libtmux_server_sessions_test
  TEST_PREFIX libtmux.server.sessions.
  PROPERTIES LABELS real-tmux TIMEOUT 60)

add_executable(libtmux_cardinality_test cardinality_test.cpp)
target_link_libraries(
  libtmux_cardinality_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux
          libtmux_test_support)
target_compile_options(
  libtmux_cardinality_test PRIVATE -fno-sanitize-recover=undefined)

gtest_discover_tests(
  libtmux_cardinality_test
  TEST_PREFIX libtmux.cardinality.
  PROPERTIES LABELS real-tmux TIMEOUT 60)

add_executable(libtmux_legacy_lookup_test legacy_lookup_test.cpp)
target_link_libraries(
  libtmux_legacy_lookup_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux
          libtmux_test_support)
target_compile_options(
  libtmux_legacy_lookup_test PRIVATE -fno-sanitize-recover=undefined)

gtest_discover_tests(
  libtmux_legacy_lookup_test
  TEST_PREFIX libtmux.legacy.
  PROPERTIES LABELS libtmux TIMEOUT 10)

add_executable(libtmux_relations_test relations_test.cpp)
target_link_libraries(
  libtmux_relations_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux
          libtmux_test_support)
target_compile_options(
  libtmux_relations_test PRIVATE -fno-sanitize-recover=undefined)

gtest_discover_tests(
  libtmux_relations_test
  TEST_PREFIX libtmux.relations.
  PROPERTIES LABELS real-tmux TIMEOUT 60)

# White-box: this one reaches into the private transport header, which is how
# it can substitute an executor at all. It links no test support because it
# never starts a tmux.
add_executable(libtmux_operation_state_test operation_state_test.cpp)
target_include_directories(
  libtmux_operation_state_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(
  libtmux_operation_state_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux)
target_compile_options(
  libtmux_operation_state_test PRIVATE -fno-sanitize-recover=undefined)

gtest_discover_tests(
  libtmux_operation_state_test
  TEST_PREFIX libtmux.operation_state.
  PROPERTIES LABELS concurrency TIMEOUT 30)

# TSan supplies strong global allocation operators of its own. The dedicated
# fault-injection executable overrides those operators, so its proofs run in
# every other lane while the queue's concurrency tests remain instrumented.
if(NOT LIBTMUX_ENABLE_THREAD_SANITIZER)
  add_executable(libtmux_completion_queue_fault_test completion_queue_fault_test.cpp)
  target_include_directories(
    libtmux_completion_queue_fault_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(
    libtmux_completion_queue_fault_test
    PRIVATE libtmux_build_options libtmux::libtmux)
  target_compile_options(
    libtmux_completion_queue_fault_test PRIVATE -fno-sanitize-recover=undefined)

  add_test(
    NAME libtmux.completion_queue.EnqueueDoesNotAllocate
    COMMAND libtmux_completion_queue_fault_test enqueue)
  add_test(
    NAME libtmux.completion_queue.InsertionFailureDestroysAfterUnlock
    COMMAND libtmux_completion_queue_fault_test insertion)
  add_test(
    NAME libtmux.completion_queue.PublicationDuringRegistrationRechecks
    COMMAND libtmux_completion_queue_fault_test registration)
  add_test(
    NAME libtmux.completion_queue.AtomicReadyInsertionDispatches
    COMMAND libtmux_completion_queue_fault_test ready)
  set_tests_properties(
    libtmux.completion_queue.EnqueueDoesNotAllocate
    libtmux.completion_queue.InsertionFailureDestroysAfterUnlock
    libtmux.completion_queue.PublicationDuringRegistrationRechecks
    libtmux.completion_queue.AtomicReadyInsertionDispatches
    PROPERTIES LABELS concurrency TIMEOUT 5)
endif()

# POSIX only: the child's signal mask and dispositions are what this proves,
# and Windows starts its children through a different launcher entirely.
if(NOT WIN32)
  add_executable(libtmux_spawn_descriptors_test spawn_descriptors_test.cpp)
  target_include_directories(
    libtmux_spawn_descriptors_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(
    libtmux_spawn_descriptors_test
    PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux)
  target_compile_options(
    libtmux_spawn_descriptors_test PRIVATE -fno-sanitize-recover=undefined)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(
      libtmux_spawn_descriptors_test
      PRIVATE LIBTMUX_SPAWN_DESCRIPTOR_TEST_SEAM)
    target_link_options(
      libtmux_spawn_descriptors_test PRIVATE "-Wl,--wrap=getrlimit")
  endif()

  gtest_discover_tests(
    libtmux_spawn_descriptors_test
    TEST_PREFIX libtmux.spawn_descriptors.
    PROPERTIES LABELS "libtmux" TIMEOUT 30)

  add_executable(libtmux_spawn_signals_test spawn_signals_test.cpp)
  target_include_directories(
    libtmux_spawn_signals_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(
    libtmux_spawn_signals_test
    PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux)
  target_compile_options(
    libtmux_spawn_signals_test PRIVATE -fno-sanitize-recover=undefined)

  gtest_discover_tests(
    libtmux_spawn_signals_test
    TEST_PREFIX libtmux.spawn_signals.
    PROPERTIES LABELS "libtmux" TIMEOUT 30)
endif()

# White-box: the child owner is private, and what it owns — pipes, signals,
# a reap — is not observable through the runner's result.
if(NOT WIN32)
  add_executable(libtmux_posix_child_test posix_child_test.cpp)
  target_include_directories(
    libtmux_posix_child_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(
    libtmux_posix_child_test
    PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux)
  target_compile_options(
    libtmux_posix_child_test PRIVATE -fno-sanitize-recover=undefined)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(
      libtmux_posix_child_test PRIVATE LIBTMUX_SPAWN_DESCRIPTOR_TEST_SEAM)
  endif()

  gtest_discover_tests(
    libtmux_posix_child_test
    TEST_PREFIX libtmux.posix_child.
    PROPERTIES LABELS "libtmux" TIMEOUT 30)
endif()

# The psmux rules are pure string logic, so they are tested wherever this
# builds rather than only where psmux runs.
add_executable(libtmux_psmux_rules_test psmux_rules_test.cpp)
target_include_directories(
  libtmux_psmux_rules_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(
  libtmux_psmux_rules_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux)
target_compile_options(
  libtmux_psmux_rules_test PRIVATE -fno-sanitize-recover=undefined)

gtest_discover_tests(
  libtmux_psmux_rules_test
  TEST_PREFIX libtmux.psmux_rules.
  PROPERTIES LABELS "libtmux" TIMEOUT 30)

# The asynchronous engine. White-box: it is private, and what it owns —
# threads, pipes, a reap — is not observable through its result alone.
if(NOT WIN32)
  add_executable(libtmux_process_engine_test process_engine_test.cpp)
  target_include_directories(
    libtmux_process_engine_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(
    libtmux_process_engine_test
    PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux)
  target_compile_options(
    libtmux_process_engine_test PRIVATE -fno-sanitize-recover=undefined)

  gtest_discover_tests(
    libtmux_process_engine_test
    TEST_PREFIX libtmux.process_engine.
    PROPERTIES LABELS "concurrency" TIMEOUT 60)

  add_executable(
    libtmux_process_engine_failure_test process_engine_failure_test.cpp)
  target_include_directories(
    libtmux_process_engine_failure_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(
    libtmux_process_engine_failure_test
    PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux)
  target_compile_options(
    libtmux_process_engine_failure_test PRIVATE -fno-sanitize-recover=undefined)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(
      libtmux_process_engine_failure_test
      PRIVATE
        "-Wl,--wrap=kill"
        "-Wl,--wrap=poll"
        "-Wl,--wrap=posix_spawn_file_actions_destroy"
        "-Wl,--wrap=posix_spawnp"
        "-Wl,--wrap=posix_spawnattr_destroy"
        "-Wl,--wrap=read"
        "-Wl,--wrap=waitpid"
        "-Wl,--wrap=write")
  endif()

  gtest_discover_tests(
    libtmux_process_engine_failure_test
    TEST_PREFIX libtmux.process_engine_failure.
    PROPERTIES LABELS "concurrency" TIMEOUT 60)
endif()

add_executable(libtmux_backend_seam_test backend_seam_test.cpp)
target_include_directories(
  libtmux_backend_seam_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(
  libtmux_backend_seam_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux)
target_compile_options(
  libtmux_backend_seam_test PRIVATE -fno-sanitize-recover=undefined)

add_test(NAME libtmux.backend_seam COMMAND libtmux_backend_seam_test)
set_tests_properties(
  libtmux.backend_seam PROPERTIES LABELS "libtmux" TIMEOUT 30)

add_executable(command_engine_exit_probe command_engine_exit_probe.cpp)
target_include_directories(
  command_engine_exit_probe PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(
  command_engine_exit_probe PRIVATE libtmux_build_options libtmux::libtmux)
add_test(NAME libtmux.command_engine.exit_detaches COMMAND command_engine_exit_probe)
set_tests_properties(
  libtmux.command_engine.exit_detaches PROPERTIES LABELS "concurrency" TIMEOUT 3)

add_executable(libtmux_client_test integration/client_test.cpp)
target_link_libraries(
  libtmux_client_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux
          libtmux_test_support)
target_compile_options(libtmux_client_test PRIVATE -fno-sanitize-recover=undefined)

# Per case: each capability the parity ledger classifies needs a test of
# its own to cite, and one name for a whole file cannot serve several.
gtest_discover_tests(
  libtmux_client_test
  TEST_PREFIX libtmux.client.
  PROPERTIES LABELS real-tmux TIMEOUT 60)

add_executable(libtmux_value_semantics_test value_semantics_test.cpp)
target_link_libraries(
  libtmux_value_semantics_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux
          libtmux_test_support)
target_compile_options(
  libtmux_value_semantics_test PRIVATE -fno-sanitize-recover=undefined)

gtest_discover_tests(
  libtmux_value_semantics_test
  TEST_PREFIX libtmux.value_semantics.
  PROPERTIES LABELS real-tmux TIMEOUT 60)

add_executable(libtmux_entity_test entity_test.cpp)
target_link_libraries(
  libtmux_entity_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux
          libtmux_test_support)
target_compile_options(libtmux_entity_test PRIVATE -fno-sanitize-recover=undefined)

gtest_discover_tests(
  libtmux_entity_test
  TEST_PREFIX libtmux.entity.
  PROPERTIES LABELS real-tmux TIMEOUT 60)

add_executable(libtmux_control_test control_test.cpp)
target_link_libraries(
  libtmux_control_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux
          libtmux_test_support)
target_compile_options(
  libtmux_control_test PRIVATE -fno-sanitize-recover=undefined)

gtest_discover_tests(
  libtmux_control_test
  TEST_PREFIX libtmux.control.
  PROPERTIES LABELS real-tmux TIMEOUT 30)

add_executable(libtmux_server_contract_test server_contract_test.cpp)
target_link_libraries(
  libtmux_server_contract_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux
          libtmux_test_support)
target_compile_options(
  libtmux_server_contract_test PRIVATE -fno-sanitize-recover=undefined)

gtest_discover_tests(
  libtmux_server_contract_test
  TEST_PREFIX libtmux.server_contract.
  PROPERTIES LABELS real-tmux TIMEOUT 60)

# The control-mode contract, moved off the prototype it was written against and
# onto the library that now ships the same connection.
#
# The prototype pinned one exact tmux binary, which is right for frozen bakeoff
# evidence and wrong here: the library supports a range, so this resolves tmux,
# records which binary ran, and checks the version is one the library claims.
find_program(LIBTMUX_TEST_TMUX NAMES tmux REQUIRED NO_CACHE)
file(SHA256 "${LIBTMUX_TEST_TMUX}" LIBTMUX_TEST_TMUX_SHA256)
execute_process(
  COMMAND "${LIBTMUX_TEST_TMUX}" -V
  RESULT_VARIABLE LIBTMUX_TEST_TMUX_RESULT
  OUTPUT_VARIABLE LIBTMUX_TEST_TMUX_VERSION
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT LIBTMUX_TEST_TMUX_RESULT EQUAL 0)
  message(FATAL_ERROR "tmux -V failed for ${LIBTMUX_TEST_TMUX}")
endif()
# `master` and `next-3.8` are what tmux built from git calls itself, and both
# are newer than every numbered release — the library's own `Version` orders
# them that way. A guard that only understands numbered releases turns the
# newest tmux there is into "too old to test".
if(NOT LIBTMUX_TEST_TMUX_VERSION MATCHES
   "^tmux (master|next-|3\\.[2-9]|3\\.[1-9][0-9]|[4-9]\\.)")
  message(
    FATAL_ERROR
      "control-mode tests need tmux 3.2 or newer, found ${LIBTMUX_TEST_TMUX_VERSION}")
endif()

# The version is no longer stamped into the support library from here.
# `libtmux::testing` asks the binary it is about to spawn, at runtime, which
# is both more honest — a configure-time answer can name a different tmux than
# the one that runs — and the thing that made the fixture installable at all.
# The check above stays: it is worth failing configuration over a tmux this
# library does not claim, rather than discovering it 200 tests later.

foreach(control_case parser integration)
  add_executable(
    libtmux_control_${control_case}_test control_${control_case}_test.cpp)
  target_link_libraries(
    libtmux_control_${control_case}_test
    PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux
            libtmux_test_support)
  target_compile_definitions(
    libtmux_control_${control_case}_test
    PRIVATE LIBTMUX_CONTROL_TMUX_PATH="${LIBTMUX_TEST_TMUX}"
            LIBTMUX_CONTROL_TMUX_SHA256="${LIBTMUX_TEST_TMUX_SHA256}")
  target_compile_options(
    libtmux_control_${control_case}_test PRIVATE -fno-sanitize-recover=undefined)
endforeach()
target_include_directories(
  libtmux_control_integration_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(
    libtmux_control_integration_test
    PRIVATE LIBTMUX_SPAWN_DESCRIPTOR_TEST_SEAM)
endif()

include(GoogleTest)
gtest_discover_tests(
  libtmux_control_parser_test
  TEST_PREFIX libtmux.control.parser.
  PROPERTIES LABELS control TIMEOUT 30)
gtest_discover_tests(
  libtmux_control_integration_test
  TEST_PREFIX libtmux.control.integration.
  PROPERTIES LABELS real-tmux TIMEOUT 60)

add_executable(libtmux_lowering_test lowering_test.cpp)
target_link_libraries(
  libtmux_lowering_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux)
target_compile_options(
  libtmux_lowering_test PRIVATE -fno-sanitize-recover=undefined)

add_test(NAME libtmux.lowering COMMAND libtmux_lowering_test)
set_tests_properties(libtmux.lowering PROPERTIES LABELS "libtmux" TIMEOUT 10)

add_executable(libtmux_schema_test lowering_structure_test.cpp)
target_link_libraries(
  libtmux_schema_test
  PRIVATE GTest::gtest_main libtmux_build_options libtmux::libtmux)
target_compile_options(libtmux_schema_test PRIVATE -fno-sanitize-recover=undefined)

# Named for what it checks. Conformance to the published schema is
# `schema.conformance`, which validates emitted documents against the file.
add_test(NAME libtmux.lowering.structure COMMAND libtmux_schema_test)
set_tests_properties(
  libtmux.lowering.structure PROPERTIES LABELS "libtmux" TIMEOUT 10)

# Compile-time contracts.
#
# Each probe below must fail to compile. CTest runs the build of a target that
# is excluded from the default build and requires it to fail, so a contract
# that quietly starts compiling is a test failure rather than silence. The
# positive control builds normally: without it, a typo in an include would make
# every probe "pass".
# An object library, not an executable: these probes are about compiling, and
# a main would only be there to satisfy the linker.
add_library(libtmux_compile_valid_uses OBJECT compile/valid_uses.cpp)
target_link_libraries(
  libtmux_compile_valid_uses PRIVATE libtmux_build_options libtmux::libtmux)

foreach(
  probe
  flag_field_has_no_string_operations
  numeric_field_has_no_string_operations
  string_field_has_no_ordering
  cardinality_refuses_a_temporary
  cardinality_refuses_a_produced_element
  capture_lines_refuses_a_temporary
  parse_refuses_a_temporary
  relation_join_refuses_a_temporary)
  add_library(libtmux_compile_${probe} OBJECT EXCLUDE_FROM_ALL compile/${probe}.cpp)
  target_link_libraries(
    libtmux_compile_${probe} PRIVATE libtmux_build_options libtmux::libtmux)

  add_test(
    NAME libtmux.compile.${probe}
    COMMAND
      "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target
      libtmux_compile_${probe} --config $<CONFIG>)
  set_tests_properties(
    libtmux.compile.${probe}
    PROPERTIES
      WILL_FAIL TRUE
      LABELS "libtmux;compile"
      TIMEOUT 120
      # These drive the build system, so they cannot share it with a
      # parallel CTest run.
      RESOURCE_LOCK build_tree)
endforeach()
