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
# Answers in three parts:
#
#   out_found        TRUE when a copy the system already has was accepted. The
#                    caller links its imported target `tl::expected`, and the
#                    installed package config finds it again for a consumer.
#                    Linking is the only way to carry it: the target's include
#                    list can hold several directories (v1.1.0 installs
#                    <prefix>/include and <prefix>/share), and copying that list
#                    into a generator expression breaks it into pieces.
#   out_include_dir  Otherwise, the include directory of the copy this build
#                    fetched, to be added by hand.
#   out_install_dir  The directory of that fetched copy to install, so a
#                    consumer finds `<tl/expected.hpp>` under this prefix.
#
# The version is not asked of find_package. The v1.1.0 release declares
# `VERSION 1.0.0` in its own `project()`, so `find_package(tl-expected 1.1)`
# rejects that release's own install. What the library needs is not a number but
# an API, and the number cannot say whether it is there, so the found copy is
# compiled against the two members the public surface and the examples use and
# is refused, with the reason, if either is missing.
function(libtmux_resolve_tl_expected out_found out_include_dir out_install_dir)
  set(${out_found} FALSE PARENT_SCOPE)
  set(${out_include_dir} "" PARENT_SCOPE)
  set(${out_install_dir} "" PARENT_SCOPE)

  find_package(tl-expected CONFIG QUIET)
  if(TARGET tl::expected)
    include(CheckCXXSourceCompiles)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_REQUIRED_LIBRARIES tl::expected)
    # The cache would otherwise keep the answer for a copy that has since been
    # replaced.
    unset(LIBTMUX_TL_EXPECTED_HAS_API CACHE)
    check_cxx_source_compiles(
      "#include <tl/expected.hpp>
       int main() {
         tl::expected<int, int> value{1};
         auto chained = value
             .and_then([](int v) { return tl::expected<int, int>{v + 1}; })
             .transform([](int v) { return v + 1; });
         return chained.value() == 3 ? 0 : 1;
       }"
      LIBTMUX_TL_EXPECTED_HAS_API)
    if(LIBTMUX_TL_EXPECTED_HAS_API)
      # Found on the system, so it is already installed and must not be
      # installed again under this prefix.
      set(${out_found} TRUE PARENT_SCOPE)
      return()
    endif()
    message(STATUS
      "tl-expected ${tl-expected_VERSION} was found but lacks and_then or "
      "transform, which libtmux needs; it comes with v1.1.0")
  endif()

  if(NOT LIBTMUX_FETCH_DEPS)
    message(FATAL_ERROR
      "tl::expected 1.1.0 or newer is required to build with LIBTMUX_CXX_STANDARD=20; "
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
