include("${PROJECT_SOURCE_DIR}/cmake/MinGWRuntime.cmake")

add_executable(libtmux_example_07_windows_psmux ../07-windows-psmux.cpp)
target_link_libraries(libtmux_example_07_windows_psmux PRIVATE libtmux::libtmux)
target_compile_features(
  libtmux_example_07_windows_psmux PRIVATE cxx_std_${LIBTMUX_CXX_STANDARD})
libtmux_copy_mingw_runtime(libtmux_example_07_windows_psmux)

if(MSVC)
  target_compile_options(libtmux_example_07_windows_psmux PRIVATE /W4 /permissive-)
  if(LIBTMUX_WARNINGS_AS_ERRORS)
    target_compile_options(libtmux_example_07_windows_psmux PRIVATE /WX)
  endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(
    libtmux_example_07_windows_psmux
    PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
  if(LIBTMUX_WARNINGS_AS_ERRORS)
    target_compile_options(libtmux_example_07_windows_psmux PRIVATE -Werror)
  endif()
endif()

if(NOT CMAKE_CROSSCOMPILING)
  find_program(
    LIBTMUX_EXAMPLE_POWERSHELL
    NAMES powershell.exe pwsh.exe pwsh
    REQUIRED)
  add_test(
    NAME example.07-windows-psmux
    COMMAND
      "${LIBTMUX_EXAMPLE_POWERSHELL}" -NoLogo -NoProfile -NonInteractive
      -ExecutionPolicy Bypass -File
      "${CMAKE_CURRENT_SOURCE_DIR}/run_psmux_example.ps1" -Example
      "$<TARGET_FILE:libtmux_example_07_windows_psmux>")
  set_tests_properties(
    example.07-windows-psmux
    PROPERTIES LABELS "example;windows;psmux;real-tmux" TIMEOUT 120)
endif()
