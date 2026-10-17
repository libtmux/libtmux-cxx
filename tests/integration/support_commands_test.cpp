// How the library reads a raw command line.
//
// The guards for raw commands decide from what the daemon would run, so these
// tests hold the reading to tmux's own: which command a word names, where one
// command ends, and which flags take a value. None of them needs a daemon.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "support_commands.hpp"
#include "support_versions.hpp"

namespace {

namespace commands = libtmux::support::commands;
using Argv = std::vector<std::string>;

TEST(CommandNames, AnExactNameAnAliasAndAUniquePrefixAllResolve) {
  EXPECT_EQ(commands::resolve("break-pane"), "break-pane");
  EXPECT_EQ(commands::resolve("breakp"), "break-pane");
  EXPECT_EQ(commands::resolve("break-p"), "break-pane");
  EXPECT_EQ(commands::resolve("break"), "break-pane");
  EXPECT_EQ(commands::resolve("delete-b"), "delete-buffer");
  EXPECT_EQ(commands::resolve("deleteb"), "delete-buffer");
  EXPECT_EQ(commands::resolve("setb"), "set-buffer");
}

TEST(CommandNames, AnAmbiguousOrUnknownWordResolvesToNothing) {
  // `b` begins both bind-key and break-pane; tmux rejects it as ambiguous.
  EXPECT_FALSE(commands::resolve("b").has_value());
  EXPECT_FALSE(commands::resolve("").has_value());
  EXPECT_FALSE(commands::resolve("breakpane").has_value());
  EXPECT_FALSE(commands::resolve("break-panes").has_value());
}

TEST(CommandNames, AnAliasIsFoundBeforeALongerPrefixCouldMakeItAmbiguous) {
  // `new` is the alias of new-session and also begins new-pane, new-window.
  EXPECT_EQ(commands::resolve("new"), "new-session");
  // `lock` is lock-server's alias and begins lock-client and lock-session.
  EXPECT_EQ(commands::resolve("lock"), "lock-server");
  EXPECT_FALSE(commands::resolve("loc").has_value());
}

TEST(CommandLists, ACommandEndsAtASemicolonElementOrAtATrailingSemicolon) {
  const auto list = commands::invocations(
      {"split-window", "-d", ";", "breakp", "-d;", "kill-pane", "-a"});
  ASSERT_EQ(list.size(), 3U);
  EXPECT_EQ(list[0].name, "split-window");
  EXPECT_EQ(list[1].name, "break-pane");
  EXPECT_EQ(list[1].arguments, (Argv{"-d"}));
  EXPECT_EQ(list[2].name, "kill-pane");
}

TEST(CommandLists, AnEscapedSemicolonIsAnArgumentAndEndsNothing) {
  const auto list = commands::invocations({"display-message", "a\\;", "break-p"});
  ASSERT_EQ(list.size(), 1U);
  EXPECT_EQ(list[0].arguments, (Argv{"a;", "break-p"}));
}

TEST(CommandLists, ACommandWhoseNameDoesNotResolveIsLeftOut) {
  const auto list = commands::invocations({"b", "-d", ";", "frobnicate", ";", "ls"});
  ASSERT_EQ(list.size(), 1U);
  EXPECT_EQ(list[0].name, "list-sessions");
}

TEST(CommandFlags, AValueIsTheRestOfTheClusterOrTheNextElement) {
  const Argv arguments{"-dn", "name", "-Fx", "--", "-n"};
  const auto flags = commands::parse_flags(arguments, "Fn");
  EXPECT_TRUE(flags.has('d'));
  EXPECT_EQ(flags.value('n'), "name");
  EXPECT_EQ(flags.value('F'), "x");
  EXPECT_EQ(flags.operands, (Argv{"-n"}));
}

TEST(CommandFlags, FlagsStopAtTheFirstOperand) {
  const Argv arguments{"-d", "layout", "-n"};
  const auto flags = commands::parse_flags(arguments, "n");
  EXPECT_TRUE(flags.has('d'));
  EXPECT_FALSE(flags.has('n'));
  EXPECT_EQ(flags.operands, (Argv{"layout", "-n"}));
}

TEST(UnnamedBreakPane, EverySpellingTmuxAcceptsIsFound) {
  using libtmux::support::versions::has_unnamed_break_pane;
  EXPECT_TRUE(has_unnamed_break_pane({"break-pane", "-d"}));
  EXPECT_TRUE(has_unnamed_break_pane({"breakp", "-d"}));
  EXPECT_TRUE(has_unnamed_break_pane({"break-p", "-d"}));
  EXPECT_TRUE(has_unnamed_break_pane({"br", "-d"}));
  EXPECT_TRUE(has_unnamed_break_pane({"split-window", ";", "break-p", "-d"}));
  EXPECT_TRUE(has_unnamed_break_pane({"split-window", "-d;", "breakp"}));
}

TEST(UnnamedBreakPane, ANamedOneAndAWordTmuxRejectsAreNotFound) {
  using libtmux::support::versions::has_unnamed_break_pane;
  EXPECT_FALSE(has_unnamed_break_pane({"break-pane", "-d", "-n", "w"}));
  EXPECT_FALSE(has_unnamed_break_pane({"break-p", "-dnw"}));
  EXPECT_FALSE(has_unnamed_break_pane({"split-window", ";", "list-panes"}));
  // Ambiguous and unknown words are rejected by tmux, with the list.
  EXPECT_FALSE(has_unnamed_break_pane({"b", "-d"}));
  EXPECT_FALSE(has_unnamed_break_pane({"breakpane", "-d"}));
  // A `-n` after `--` is an operand, not a flag.
  EXPECT_TRUE(has_unnamed_break_pane({"break-pane", "--", "-n"}));
}

} // namespace
