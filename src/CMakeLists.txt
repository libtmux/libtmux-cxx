# Before any use of CMAKE_INSTALL_INCLUDEDIR. Reading it earlier yields an
# empty string, which silently exports a package with no include directory.
include(GNUInstallDirs)

add_library(
  libtmux STATIC version.cpp server.cpp backend.cpp control_backend.cpp
  snapshot.cpp entities.cpp socket_identity.cpp
  process.cpp control.cpp connection.cpp)
add_library(libtmux::libtmux ALIAS libtmux)

# Two threads per control connection. This links on a glibc that folded
# libpthread into libc, and fails to link on one that did not.
find_package(Threads REQUIRED)
target_link_libraries(libtmux PUBLIC Threads::Threads)

# Not left to the compiler's default. A static library exists to be embedded,
# including into shared objects and language bindings, and a toolchain that
# does not default to position-independent code would otherwise produce an
# archive that cannot enter one.
set_target_properties(libtmux PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(libtmux PROPERTIES OUTPUT_NAME tmuxpp EXPORT_NAME libtmux)
target_compile_features(libtmux PUBLIC cxx_std_${LIBTMUX_CXX_STANDARD})

# The shipped code is held to the same warnings as the tests, but privately:
# a consumer's build should never inherit our -Werror and fail on a warning
# their compiler happens to emit and ours does not.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(
    libtmux
    PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
  if(LIBTMUX_WARNINGS_AS_ERRORS)
    target_compile_options(libtmux PRIVATE -Werror)
  endif()
endif()

if(LIBTMUX_CXX_STANDARD STREQUAL "20")
  include("${PROJECT_SOURCE_DIR}/cmake/TlExpected.cmake")
  libtmux_resolve_tl_expected(LIBTMUX_TL_EXPECTED_INCLUDE LIBTMUX_TL_EXPECTED_INSTALL)
  # Never linked, only included. tl::expected is header-only, so linking it
  # adds nothing but would put the resolved target in our exported link
  # interface, where a consumer cannot resolve it.
  target_include_directories(
    libtmux PUBLIC $<BUILD_INTERFACE:${LIBTMUX_TL_EXPECTED_INCLUDE}>)
  target_compile_definitions(libtmux PUBLIC LIBTMUX_USE_TL_EXPECTED)
  if(LIBTMUX_INSTALL AND LIBTMUX_TL_EXPECTED_INSTALL)
    # Only the copy this build fetched. One the system already provides is
    # already installed, and copying it under this prefix would shadow it.
    install(DIRECTORY "${LIBTMUX_TL_EXPECTED_INSTALL}"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  endif()
endif()
# The full version, prerelease tag and all — `PROJECT_VERSION` has had the tag
# stripped so CMake would accept it, and a library that reports "0.1.0" while
# it is an alpha is telling the truth to nobody.
target_compile_definitions(
  libtmux PRIVATE LIBTMUX_VERSION="${LIBTMUX_VERSION_FULL}")
target_include_directories(
  libtmux
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# The library must carry the same sanitizer as anything linking it. Building
# it clean while the tests are instrumented makes libc++'s container
# annotations disagree, which surfaces as a container-overflow inside a
# perfectly correct insert.
if(LIBTMUX_ENABLE_COVERAGE)
  # The library carries the instrumentation too, or the report covers only the
  # headers the tests inlined.
  target_compile_options(libtmux PRIVATE -fprofile-instr-generate -fcoverage-mapping)
  target_link_options(libtmux PUBLIC -fprofile-instr-generate -fcoverage-mapping)
endif()

if(LIBTMUX_ENABLE_SANITIZERS)
  target_compile_options(libtmux PRIVATE -fsanitize=address,undefined)
  target_link_options(libtmux PUBLIC -fsanitize=address,undefined)
elseif(LIBTMUX_ENABLE_THREAD_SANITIZER)
  target_compile_options(libtmux PRIVATE -fsanitize=thread)
  target_link_options(libtmux PUBLIC -fsanitize=thread)
endif()

if(LIBTMUX_ENABLE_CLANG_TIDY)
  find_program(LIBTMUX_CLANG_TIDY NAMES clang-tidy REQUIRED NO_CACHE)
  # Only the library. Running the curated checks over test sources would
  # report on GoogleTest's macros rather than on code we ship.
  set_target_properties(
    libtmux PROPERTIES CXX_CLANG_TIDY "${LIBTMUX_CLANG_TIDY}")
endif()

if(LIBTMUX_INSTALL)
  include(CMakePackageConfigHelpers)

  install(
    TARGETS libtmux
    EXPORT libtmuxTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  # `testing/` is excluded and installed by `src/testing` instead. Headers for
  # a target that was not built would be an offer the package cannot keep: a
  # consumer who includes one gets a link error rather than the configure-time
  # refusal the component is there to give them.
  install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/libtmux"
          DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
          PATTERN "testing" EXCLUDE)
  install(
    EXPORT libtmuxTargets
    NAMESPACE libtmux::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/libtmux")

  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/libtmuxConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    # SameMinorVersion, not SameMajorVersion: before 1.0 the minor is where
    # breaking changes land, so 0.1 must not satisfy a request for 0.2.
    COMPATIBILITY SameMinorVersion)
  configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/libtmuxConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/libtmuxConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/libtmux")
  install(FILES "${PROJECT_SOURCE_DIR}/LICENSE"
          DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/licenses/libtmux")
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/libtmuxConfig.cmake"
                "${CMAKE_CURRENT_BINARY_DIR}/libtmuxConfigVersion.cmake"
          DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/libtmux")
endif()
