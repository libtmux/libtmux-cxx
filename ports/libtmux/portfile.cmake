# The library is not header-only and installs a CMake package config, so the
# standard cmake helpers do the whole job.
vcpkg_from_github(
  OUT_SOURCE_PATH SOURCE_PATH
  REPO libtmux/libtmux-cxx
  REF "v${VERSION}"
  SHA512 18f95a66fd23ae79587ebbf84d18a637c414249f0af9a2dbb124d52851ae35575fdacb886e916d43af91e1a1014f112c5118d8ccfa4a700b4fb7d9ce7256c586
  HEAD_REF master)

vcpkg_check_features(
  OUT_FEATURE_OPTIONS FEATURE_OPTIONS
  FEATURES mcp LIBTMUX_BUILD_MCP_SERVER)

# The private-server fixture needs POSIX process and socket APIs, so it cannot
# follow the library onto Windows when the `supports` clause admits it.
if(VCPKG_TARGET_IS_WINDOWS)
  set(LIBTMUX_PORT_BUILD_TESTING_LIBRARY OFF)
else()
  set(LIBTMUX_PORT_BUILD_TESTING_LIBRARY ON)
endif()

vcpkg_cmake_configure(
  SOURCE_PATH "${SOURCE_PATH}"
  OPTIONS
    ${FEATURE_OPTIONS}
    # This project's own tests need GoogleTest and a real tmux, and its
    # examples need a tmux to run against. Neither belongs in a consumer's
    # dependency graph.
    -DLIBTMUX_BUILD_TESTS=OFF
    -DLIBTMUX_BUILD_EXAMPLES=OFF
    # `libtmux::testing` does belong there, and follows those two switches
    # unless it is asked for. It is the fixture a consumer's own suite runs
    # on — a private tmux server per test — and it names no test framework and
    # links nothing beyond the library, so shipping it costs a consumer an
    # archive they never link unless they ask for the component.
    -DLIBTMUX_BUILD_TESTING_LIBRARY=${LIBTMUX_PORT_BUILD_TESTING_LIBRARY}
    # Every dependency this port does not declare must fail the build rather
    # than reach the network for a pinned fallback.
    -DLIBTMUX_FETCH_DEPS=OFF
    # Pinned, not left to the upstream default: under C++20 the package
    # substitutes `tl::expected`, which this port does not depend on.
    -DLIBTMUX_CXX_STANDARD=23)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME libtmux CONFIG_PATH lib/cmake/libtmux)

if("mcp" IN_LIST FEATURES)
  # A program, so it moves out of `bin/` — which a static triplet does not
  # otherwise have — and into the port's own tools directory.
  vcpkg_copy_tools(TOOL_NAMES libtmux-mcp-server AUTO_CLEAN)
endif()

# Headers come from the release tree, and so does the license: the project
# installs it to `share/licenses/libtmux/` for an ordinary `cmake --install`,
# which is right there and wrong here — vcpkg keeps a port's copyright at
# `share/libtmux/copyright` and rejects a `debug/share` outright.
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include"
                    "${CURRENT_PACKAGES_DIR}/debug/share"
                    "${CURRENT_PACKAGES_DIR}/share/licenses")

# The generated usage text names `libtmux::testing` beside the library, but the
# package config defines that target only for `COMPONENTS testing` — pasting
# what vcpkg would generate fails to configure.
configure_file("${CMAKE_CURRENT_LIST_DIR}/usage"
               "${CURRENT_PACKAGES_DIR}/share/${PORT}/usage" COPYONLY)
if("mcp" IN_LIST FEATURES)
  file(READ "${CMAKE_CURRENT_LIST_DIR}/usage-mcp" LIBTMUX_USAGE_MCP)
  file(APPEND "${CURRENT_PACKAGES_DIR}/share/${PORT}/usage" "${LIBTMUX_USAGE_MCP}")
endif()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
