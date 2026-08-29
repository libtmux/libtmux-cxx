// The build is the configuration it claims to be.
//
// Two configurations ship: C++23 over std::expected, and C++20 over the pinned
// compatibility type. Each has its own inline ABI namespace, so an object from
// one cannot silently link against the other — and each has to be able to say
// which one it is.

#include "libtmux/abi.hpp"
#include "libtmux/expected.hpp"
#include "libtmux/version.hpp"

#include <cstddef>

#include <gtest/gtest.h>

// Only when the pinned toolchain was chosen. That build asks for a specific
// clang paired with a specific libc++, and getting libstdc++ or a different
// libc++ instead would be a silent change of what is under test. Apple Clang
// brings its own libc++ at its own version and is not that pairing, so a bare
// `__clang__` check called a correct macOS build broken.
#if defined(LIBTMUX_PINNED_LIBCXX)
#if !defined(_LIBCPP_VERSION) || _LIBCPP_VERSION != 180100
#error "the pinned toolchain is clang 18.1 with libc++ 18.1"
#endif
#endif

#if defined(LIBTMUX_USE_TL_EXPECTED)
#include <tl/expected.hpp>
#else
#include <expected>
#endif

TEST(BuildSmoke, TheExpectedBackendIsTheOneThisStandardSelects) {
#if defined(LIBTMUX_USE_TL_EXPECTED)
  static_assert(std::is_same_v<libtmux::expected<int, int>, tl::expected<int, int>>,
                "the C++20 configuration selects the compatibility type");
#else
  static_assert(__cpp_lib_expected >= 202202L);
  static_assert(std::is_same_v<libtmux::expected<int, int>, std::expected<int, int>>,
                "the C++23 configuration selects the standard type");
#endif

  libtmux::expected<int, int> value{42};
  EXPECT_EQ(*value, 42);

  const auto failed = libtmux::expected<int, int>{libtmux::unexpected(7)};
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error(), 7);
}

TEST(BuildSmoke, TheAbiNamespaceNamesTheBackend) {
  // Spelled through the inline namespace, which is how an object built one way
  // fails to link against a library built the other rather than reading the
  // wrong bytes.
#if defined(LIBTMUX_USE_TL_EXPECTED)
  const libtmux::v2_cxx20::Version version{.major = 3, .minor = 4};
#else
  const libtmux::v2_cxx23::Version version{.major = 3, .minor = 4};
#endif
  EXPECT_EQ(version.major, 3U);
  EXPECT_TRUE(libtmux::is_supported(version));
}

TEST(BuildSmoke, PsmuxVersionUsesTheExistingRevisionSlot) {
  const auto version =
      libtmux::parse_version("tmux 3.3.7\r\npsmux 3.3.7 (05cc5d4 2026-07-20)\r\n");
  const auto tmux_revision = libtmux::parse_version("tmux 3.3g");
  ASSERT_TRUE(version.has_value());
  ASSERT_TRUE(tmux_revision.has_value());
  EXPECT_EQ(version->major, 3U);
  EXPECT_EQ(version->minor, 3U);
  EXPECT_EQ(version->revision, 7U);
  EXPECT_EQ(tmux_revision->revision, 7U);
  EXPECT_EQ(*version, *tmux_revision);
  EXPECT_TRUE(libtmux::is_supported(*version));
}

TEST(BuildSmoke, VersionLayoutAndNumericBoundsStayStable) {
  static_assert(sizeof(libtmux::Version) == 16U);
  static_assert(offsetof(libtmux::Version, revision) == 8U);
  static_assert(offsetof(libtmux::Version, prerelease) == 12U);
  static_assert(offsetof(libtmux::Version, unbounded) == 13U);

  EXPECT_FALSE(libtmux::parse_version("tmux 4294967296.1").has_value());
  EXPECT_FALSE(libtmux::parse_version("tmux 3.4294967296").has_value());
  EXPECT_FALSE(libtmux::parse_version("tmux 3.3.4294967296").has_value());
}
