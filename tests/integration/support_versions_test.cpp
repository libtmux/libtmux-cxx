// The tmux version ordering the support floor rests on.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "libtmux/testing/capabilities.hpp"
#include "libtmux/version.hpp"

namespace {
using libtmux::Version;


Version parsed(std::string_view text) {
  const auto version = libtmux::parse_version("tmux " + std::string{text});
  EXPECT_TRUE(version.has_value()) << text;
  return version.value_or(Version{});
}

TEST(SupportVersions, TheFloorIsTheCompleteReleaseIncludingItsLetter) {
  EXPECT_EQ(libtmux::kMinimumSupported, parsed("3.3a"));
  // Every message names the floor from this text.
  EXPECT_EQ(libtmux::kMinimumSupported, parsed(libtmux::kMinimumSupportedText));
  EXPECT_FALSE(libtmux::is_supported(parsed("3.2a")));
  EXPECT_FALSE(libtmux::is_supported(parsed("3.3")));
  EXPECT_TRUE(libtmux::is_supported(parsed("3.3a")));
  EXPECT_TRUE(libtmux::is_supported(parsed("3.3b")));
  EXPECT_TRUE(libtmux::is_supported(parsed("3.4")));
  EXPECT_TRUE(libtmux::is_supported(parsed("3.10")));
  EXPECT_TRUE(libtmux::is_supported(parsed("4.0")));
  EXPECT_FALSE(libtmux::is_supported(parsed("2.9a")));
}

TEST(SupportVersions, DevelopmentBuildsOrderBelowTheReleaseTheyLeadTo) {
  EXPECT_LT(parsed("next-3.3"), parsed("3.3a"));
  EXPECT_FALSE(libtmux::is_supported(parsed("next-3.3")));
  EXPECT_TRUE(libtmux::is_supported(parsed("next-3.4")));
  EXPECT_LT(parsed("next-3.9"), parsed("3.9"));
  EXPECT_LT(parsed("3.8-rc"), parsed("3.8"));
  EXPECT_LT(parsed("3.8-rc"), parsed("3.8-rc2"));
  EXPECT_LT(parsed("3.8-rc2"), parsed("3.8"));
  EXPECT_LT(parsed("next-3.8"), parsed("3.8-rc"));
  EXPECT_GT(parsed("3.8-rc"), parsed("3.7c"));
  EXPECT_TRUE(libtmux::is_supported(parsed("3.4-rc")));
  // The JSON layout gate is the earliest candidate measured to send it.
  const Version json_layouts{.major = 3, .minor = 8, .candidate = 2};
  EXPECT_TRUE(parsed("3.8-rc2") >= json_layouts);
  EXPECT_TRUE(parsed("3.8-rc3") >= json_layouts);
  EXPECT_TRUE(parsed("3.8") >= json_layouts);
  EXPECT_FALSE(parsed("3.8-rc") >= json_layouts);
  EXPECT_FALSE(parsed("next-3.8") >= json_layouts);
  EXPECT_FALSE(parsed("3.7c") >= json_layouts);
}

TEST(SupportVersions, ACandidateSkipAdmitsTheEarliestBuildThatHasTheCapability) {
  using libtmux::test::lacks_tmux;
  EXPECT_FALSE(lacks_tmux(parsed("3.8-rc2"), 3, 8, 2));
  EXPECT_FALSE(lacks_tmux(parsed("3.8-rc3"), 3, 8, 2));
  EXPECT_FALSE(lacks_tmux(parsed("3.8"), 3, 8, 2));
  EXPECT_TRUE(lacks_tmux(parsed("3.8-rc"), 3, 8, 2));
  EXPECT_TRUE(lacks_tmux(parsed("next-3.8"), 3, 8, 2));
  EXPECT_TRUE(lacks_tmux(parsed("3.7c"), 3, 8, 2));
  // Without a candidate the gate is the release, and a candidate ranks below it.
  EXPECT_TRUE(lacks_tmux(parsed("3.8-rc2"), 3, 8));
  EXPECT_FALSE(lacks_tmux(parsed("3.8"), 3, 8));
}

TEST(SupportVersions, TheCanonicalOrderingHoldsFromDevelopmentBuildToPatchLetter) {
  const std::vector<std::vector<std::string>> chains{
      {"next-3.7", "3.7-rc", "3.7-rc3", "3.7", "3.7a", "3.7d"},
      {"next-3.4", "3.4-rc", "3.4"},
      {"3.9", "3.10", "4.0"}};
  for (const auto& chain : chains) {
    for (std::size_t lower = 0; lower < chain.size(); ++lower) {
      for (std::size_t higher = lower + 1; higher < chain.size(); ++higher) {
        EXPECT_LT(parsed(chain[lower]), parsed(chain[higher]))
            << chain[lower] << " < " << chain[higher];
      }
    }
  }
  // The gates written as literals use that same order.
  const Version control{.major = 3, .minor = 7};
  EXPECT_GE(parsed("3.8-rc2"), control);
  EXPECT_GE(parsed("next-3.8"), control);
  EXPECT_LT(parsed("next-3.7"), control);
  EXPECT_LT(parsed("3.7-rc2"), control);
  EXPECT_LT(parsed("3.4-rc"), (Version{.major = 3, .minor = 4}));
  EXPECT_LT(parsed("3.5-rc"), (Version{.major = 3, .minor = 5}));
  EXPECT_NE(parsed("3.7-rc"), control);
  EXPECT_NE(parsed("3.7a"), control);
}

TEST(SupportVersions, FormsTmuxNeverReportsAreErrorsNotVersions) {
  // Neither parse_version nor is_supported may turn these into a release:
  // `master` and the `-master` and `-dev` spellings come from wrappers, and a
  // `next-` build has no patch letter. The class of each is exact: text that
  // does not begin with the prefix is a different error from a version that
  // cannot be ordered.
  using libtmux::VersionError;
  const std::vector<std::pair<std::string, VersionError>> errors{
      {"", VersionError::missing_prefix},
      {"3.3a", VersionError::missing_prefix},
      {"tmux3.3a", VersionError::missing_prefix},
      {"openbsd-7.6", VersionError::missing_prefix},
      {"tmux 3.4garbage", VersionError::malformed},
      {"tmux 9.9alpha", VersionError::malformed},
      {"tmux 3.8-rc0", VersionError::malformed},
      {"tmux 3.8-rcx", VersionError::malformed},
      {"tmux ", VersionError::malformed},
      {"tmux 3.", VersionError::malformed},
      {"tmux .3", VersionError::malformed},
      {"tmux three", VersionError::malformed},
      {"tmux master", VersionError::malformed},
      {"tmux 3.3a-master", VersionError::malformed},
      {"tmux 3.3a-dev", VersionError::malformed},
      {"tmux next-3.3a", VersionError::malformed},
      {"tmux next-3.4a", VersionError::malformed},
      {"tmux next-3.9-master", VersionError::malformed},
      {"tmux next-3.9-rc2", VersionError::malformed},
      {"tmux openbsd-7.6", VersionError::malformed},
      {"tmux 3.3a garbage", VersionError::malformed}};
  for (const auto& [text, expected] : errors) {
    SCOPED_TRACE(text);
    const auto parsed = libtmux::parse_version(text);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error(), expected);
  }
  EXPECT_TRUE(libtmux::parse_version("tmux next-3.4").has_value());
}
} // namespace
