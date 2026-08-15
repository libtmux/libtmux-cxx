include("${PROJECT_SOURCE_DIR}/cmake/YamlCpp.cmake")
libtmux_resolve_yaml_cpp()

# The builder itself needs nothing but the library. Reading tmuxp documents
# needs a YAML parser, which is why that part is compiled rather than inline:
# a consumer of the builder does not have to see yaml-cpp at all.
add_library(workspace_builder STATIC src/tmuxp.cpp)
target_include_directories(
  workspace_builder PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(workspace_builder PUBLIC libtmux::libtmux)
target_link_libraries(workspace_builder PRIVATE yaml-cpp::yaml-cpp)

# Reads a corpus of real tmuxp documents and reports which this parser
# understands. Not a test: refusals are its output.
add_executable(tmuxp_corpus_probe tools/corpus_probe.cpp)
target_link_libraries(
  tmuxp_corpus_probe PRIVATE libtmux_build_options workspace_builder)

add_executable(workspace_builder_test tests/workspace_test.cpp tests/tmuxp_test.cpp)
target_link_libraries(
  workspace_builder_test
  PRIVATE GTest::gtest_main libtmux_build_options workspace_builder libtmux_test_support)
target_compile_options(workspace_builder_test PRIVATE -fno-sanitize-recover=undefined)

add_test(NAME consumer.workspace COMMAND workspace_builder_test)
set_tests_properties(
  consumer.workspace PROPERTIES LABELS "consumer;real-tmux" TIMEOUT 60)

# Catching a yaml-cpp parse error means catching a std::runtime_error, and the
# distribution's libc++abi is not instrumented: it frees the exception message
# with free() after operator new allocated it, which AddressSanitizer reports
# as a mismatch. A three-line program that throws and catches a
# std::runtime_error reproduces it with no yaml-cpp and no libtmux, so the
# report is about the system library rather than about anything here. Appended
# rather than assigned, so it cannot drop options set around it.
set_property(
  TEST consumer.workspace
  APPEND
  PROPERTY ENVIRONMENT_MODIFICATION
           "ASAN_OPTIONS=string_append::alloc_dealloc_mismatch=0")
