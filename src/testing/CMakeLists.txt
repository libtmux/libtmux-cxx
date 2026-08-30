# `libtmux::testing` — its own archive, export set and `find_package`
# component, so a program that only uses `libtmux` links none of it. It links
# no test framework; `capabilities.hpp` is a header to include, not a library.
add_library(
  libtmux_testing STATIC
  scoped_server.cpp
  process.cpp
  tmux_version.cpp)
add_library(libtmux::testing ALIAS libtmux_testing)

set_target_properties(
  libtmux_testing PROPERTIES OUTPUT_NAME tmuxpp-testing EXPORT_NAME testing
                             POSITION_INDEPENDENT_CODE ON)

target_include_directories(
  libtmux_testing
  PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
         "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  # `process.hpp` is implementation, not part of what this target promises.
  # The parent directory is on the path for `spawn_signals.hpp`; a quoted
  # include still finds this directory's own `process.hpp` first.
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")

target_link_libraries(libtmux_testing PUBLIC libtmux::libtmux)
target_compile_features(libtmux_testing PUBLIC cxx_std_${LIBTMUX_CXX_STANDARD})

# Privately: a consumer's build must not inherit these.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(
    libtmux_testing
    PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
  if(LIBTMUX_WARNINGS_AS_ERRORS)
    target_compile_options(libtmux_testing PRIVATE -Werror)
  endif()
endif()

if(LIBTMUX_ENABLE_SANITIZERS)
  target_compile_options(libtmux_testing PRIVATE -fsanitize=address,undefined)
  target_link_options(libtmux_testing PUBLIC -fsanitize=address,undefined)
elseif(LIBTMUX_ENABLE_THREAD_SANITIZER)
  target_compile_options(libtmux_testing PRIVATE -fsanitize=thread)
  target_link_options(libtmux_testing PUBLIC -fsanitize=thread)
endif()

if(LIBTMUX_INSTALL)
  install(
    TARGETS libtmux_testing
    EXPORT libtmuxTestingTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  # Installed with the target, so an install without it offers no headers for
  # a library that is not there. `src/CMakeLists.txt` excludes this directory.
  install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/libtmux/testing"
          DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/libtmux")
  install(
    EXPORT libtmuxTestingTargets
    NAMESPACE libtmux::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/libtmux")
endif()
