add_executable(
  differential_wire_contracts
  wire_protocol_test.cpp
  ../support/differential_wire.cpp)
target_include_directories(
  differential_wire_contracts PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(differential_wire_contracts PRIVATE cxx_std_23)
target_link_libraries(
  differential_wire_contracts PRIVATE GTest::gtest_main libtmux_build_options
          libtmux::libtmux)

add_test(NAME differential.wire.protocol COMMAND differential_wire_contracts)
set_tests_properties(
  differential.wire.protocol PROPERTIES LABELS "contract" TIMEOUT 30)
