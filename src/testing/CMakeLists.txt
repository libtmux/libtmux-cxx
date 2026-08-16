# The test-support library, shipped.
#
# Separate from `libtmux` on purpose. A program that only *uses* the library
# links none of this: it is an archive of its own, in its own export set,
# behind its own `find_package` component. Asking for it is opting in.
#
# It knows nothing about any test framework. `ScopedTmuxServer` is a fixture in
# the plain sense — construct it, get a private tmux, let it go out of scope —
# so a suite on GoogleTest, Catch2 or doctest uses the same one. Only
# `capabilities.hpp` names a framework, and it is a header the consumer chooses
# to include, not a link-time dependency this target imposes.
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
  # `process.hpp` is implementation. It spawns and reaps with pidfd and is the
  # sharpest thing in this repository; exposing it would be promising a
  # subprocess API this project does not want to own.
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(libtmux_testing PUBLIC libtmux::libtmux)
target_compile_features(libtmux_testing PUBLIC cxx_std_${LIBTMUX_CXX_STANDARD})

# Warnings privately, exactly as the library does it: a consumer's test build
# must not inherit our -Werror.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(
    libtmux_testing
    PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
  if(LIBTMUX_WARNINGS_AS_ERRORS)
    target_compile_options(libtmux_testing PRIVATE -Werror)
  endif()
endif()

# Sanitizers follow the library's lanes so an instrumented build of the suite
# does not link an uninstrumented fixture.
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
  # The headers travel with the target, not with the library's include tree —
  # `src/CMakeLists.txt` excludes this directory for exactly that reason.
  install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/libtmux/testing"
          DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/libtmux")
  # Its own export set and its own file, so `find_package(libtmux)` without the
  # component does not drag a test archive into a production link line.
  install(
    EXPORT libtmuxTestingTargets
    NAMESPACE libtmux::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/libtmux")
endif()
