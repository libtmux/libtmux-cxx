include("${PROJECT_SOURCE_DIR}/cmake/MinGWRuntime.cmake")

add_executable(libtmux_windows_psmux_smoke psmux_smoke.cpp)
target_include_directories(
  libtmux_windows_psmux_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(
  libtmux_windows_psmux_smoke PRIVATE libtmux::libtmux userenv advapi32)
target_compile_features(
  libtmux_windows_psmux_smoke PRIVATE cxx_std_${LIBTMUX_CXX_STANDARD})
libtmux_copy_mingw_runtime(libtmux_windows_psmux_smoke)

if(MSVC)
  target_compile_options(libtmux_windows_psmux_smoke PRIVATE /W4 /permissive-)
  if(LIBTMUX_WARNINGS_AS_ERRORS)
    target_compile_options(libtmux_windows_psmux_smoke PRIVATE /WX)
  endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(
    libtmux_windows_psmux_smoke
    PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
  if(LIBTMUX_WARNINGS_AS_ERRORS)
    target_compile_options(libtmux_windows_psmux_smoke PRIVATE -Werror)
  endif()
endif()

add_test(NAME windows.psmux-smoke COMMAND libtmux_windows_psmux_smoke)
set_tests_properties(
  windows.psmux-smoke
  PROPERTIES LABELS "windows;psmux;real-tmux" TIMEOUT 120)

add_executable(libtmux_windows_process_smoke process_smoke.cpp)
target_include_directories(
  libtmux_windows_process_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(libtmux_windows_process_smoke PRIVATE libtmux::libtmux)
target_compile_features(
  libtmux_windows_process_smoke PRIVATE cxx_std_${LIBTMUX_CXX_STANDARD})
libtmux_copy_mingw_runtime(libtmux_windows_process_smoke)
if(MINGW)
  target_link_options(libtmux_windows_process_smoke PRIVATE -municode)
endif()

if(MSVC)
  target_compile_options(libtmux_windows_process_smoke PRIVATE /W4 /permissive-)
  if(LIBTMUX_WARNINGS_AS_ERRORS)
    target_compile_options(libtmux_windows_process_smoke PRIVATE /WX)
  endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(
    libtmux_windows_process_smoke
    PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
  if(LIBTMUX_WARNINGS_AS_ERRORS)
    target_compile_options(libtmux_windows_process_smoke PRIVATE -Werror)
  endif()
endif()

add_test(NAME windows.process-smoke COMMAND libtmux_windows_process_smoke)
set_tests_properties(
  windows.process-smoke PROPERTIES LABELS "windows;process" TIMEOUT 30)
