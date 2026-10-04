cmake_minimum_required(VERSION 3.25)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

foreach(
  program
  server
  listings
  relations
  new-session
  new-window-pane
  query
  input-capture)
  add_executable(api-${program} "${program}.cpp")
  target_link_libraries(api-${program} PRIVATE libtmux_example_support)
  add_test(
    NAME example.api.${program}
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/docs/check_api_examples.py"
            --binary-dir "$<TARGET_FILE_DIR:api-${program}>"
            --program "${program}")
  set_tests_properties(
    example.api.${program} PROPERTIES LABELS "example;real-tmux" TIMEOUT 90)
endforeach()
