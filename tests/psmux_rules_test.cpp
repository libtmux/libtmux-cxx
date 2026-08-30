// The psmux naming rules, which no POSIX build could reach until they stopped
// being conditional. They exist to keep a name from re-parsing as a target.
#include "psmux.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace {

using libtmux_psmux::rules::invalid_session_name;
using libtmux_psmux::rules::invalid_socket_name;

TEST(PsmuxRules, RejectsASessionNameEndingInATargetSuffix) {
  EXPECT_TRUE(invalid_session_name("work.5").has_value());
  // psmux parses `session:window.pane`, so the suffix that matters is the last
  // one. A name with an earlier dot ends in one just the same.
  EXPECT_TRUE(invalid_session_name("a.b.5").has_value());
  EXPECT_TRUE(invalid_session_name("build.stage.12").has_value());
}

TEST(PsmuxRules, KeepsADottedNameThatEndsInNoSuffix) {
  EXPECT_FALSE(invalid_session_name("a.b").has_value());
  EXPECT_FALSE(invalid_session_name("release.candidate").has_value());
}

TEST(PsmuxRules, RejectsNamesWindowsCannotStore) {
  EXPECT_TRUE(invalid_session_name("con").has_value());
  EXPECT_TRUE(invalid_session_name("COM1.log").has_value());
  EXPECT_TRUE(invalid_session_name("trailing.").has_value());
  EXPECT_TRUE(invalid_session_name("two__parts").has_value());
  EXPECT_FALSE(invalid_session_name("com10").has_value());
}

TEST(PsmuxRules, RejectsASocketNameItCannotDistinguish) {
  EXPECT_TRUE(invalid_socket_name("default").has_value());
  EXPECT_TRUE(invalid_socket_name("DEFAULT").has_value());
  EXPECT_FALSE(invalid_socket_name("scratch").has_value());
}

} // namespace
