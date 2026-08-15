# The library is not header-only and installs a CMake package config, so the
# standard cmake helpers do the whole job.
vcpkg_from_github(
  OUT_SOURCE_PATH SOURCE_PATH
  REPO tmux-python/libtmux
  REF "v${VERSION}"
  SHA512 0
  HEAD_REF master)

vcpkg_cmake_configure(
  SOURCE_PATH "${SOURCE_PATH}/cxx"
  OPTIONS
    # A package build ships the library alone: tests need GoogleTest and a
    # real tmux, neither of which belongs in a consumer's dependency graph.
    -DLIBTMUX_BUILD_TESTS=OFF)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME libtmux CONFIG_PATH lib/cmake/libtmux)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
