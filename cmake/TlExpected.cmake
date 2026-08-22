include(FetchContent)

# `std::expected` is C++23. Under C++20 the package substitutes the reference
# implementation it was modelled on, and carries that choice in its compiled
# identity so a mismatched link fails rather than reading the wrong bytes.
#
# Same order as every other dependency here, and for the same reason: this one
# used to call `FetchContent_MakeAvailable` unconditionally, so the C++20 lane
# downloaded during a build that had said `LIBTMUX_FETCH_DEPS=OFF` and meant
# it. It also ignored a copy the system already had, which is the packager's
# usual complaint.
#
# Answers with the include directory to add, which is all a header-only
# dependency contributes: linking the imported target would put it in this
# package's exported link interface, where a consumer cannot resolve it.
function(libtmux_resolve_tl_expected out_include_dir out_install_dir)
  find_package(tl-expected 1.1 CONFIG QUIET)
  if(TARGET tl::expected)
    get_target_property(resolved tl::expected INTERFACE_INCLUDE_DIRECTORIES)
    set(${out_include_dir} "${resolved}" PARENT_SCOPE)
    # Found on the system, so it is already installed and must not be
    # installed again under this prefix.
    set(${out_install_dir} "" PARENT_SCOPE)
    return()
  endif()

  if(NOT LIBTMUX_FETCH_DEPS)
    message(FATAL_ERROR
      "tl::expected 1.1 is required to build with LIBTMUX_CXX_STANDARD=20; "
      "install it, or set LIBTMUX_FETCH_DEPS=ON to use the pinned fallback, "
      "or build with the C++23 default which needs no dependency at all")
  endif()

  FetchContent_Declare(
    tl_expected
    URL https://github.com/TartanLlama/expected/archive/refs/tags/v1.1.0.tar.gz
    URL_HASH
      SHA256=1db357f46dd2b24447156aaf970c4c40a793ef12a8a9c2ad9e096d9801368df6
    SOURCE_SUBDIR _libtmux_header_only)
  # A nonexistent source subdirectory populates the headers without importing
  # the dependency's unconditional package-install rules.
  FetchContent_MakeAvailable(tl_expected)
  set(${out_include_dir} "${tl_expected_SOURCE_DIR}/include" PARENT_SCOPE)
  set(${out_install_dir} "${tl_expected_SOURCE_DIR}/include/tl" PARENT_SCOPE)
endfunction()
