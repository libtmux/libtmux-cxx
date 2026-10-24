// How the library reads a raw command line.
//
// The guards for raw commands decide from what the daemon would run, so these
// tests hold the reading to tmux's own: which command a word names, where one
// command ends, and which flags take a value. None of them needs a daemon.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "libtmux/command.hpp"
#include "libtmux/version.hpp"
#include "support_commands.hpp"

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

libtmux::Version release(std::string_view text) {
  const auto version = libtmux::parse_version("tmux " + std::string{text});
  EXPECT_TRUE(version.has_value()) << text;
  return version.value_or(libtmux::Version{});
}

// Whether the list is refused on the release, as a live daemon.
std::optional<libtmux::CommandFailure> refusal(std::string_view tmux, const Argv& argv,
                                               bool live = true) {
  return commands::refuse_literal(argv, release(tmux), live);
}

TEST(LiteralBreakPane, EverySpellingTmuxAcceptsIsRefusedOnExactly37) {
  for (const Argv& argv :
       {Argv{"break-pane", "-d"}, Argv{"breakp", "-d"}, Argv{"break-p", "-d"},
        Argv{"br", "-d"}, Argv{"split-window", ";", "break-p", "-d"},
        Argv{"split-window", "-d;", "breakp"}}) {
    const auto refused = refusal("3.7", argv);
    ASSERT_TRUE(refused.has_value()) << argv.front();
    EXPECT_EQ(refused->kind, libtmux::FailureKind::validation);
    EXPECT_EQ(refused->delivery, libtmux::DeliveryStatus::not_started);
    EXPECT_FALSE(refusal("3.7a", argv)) << argv.front();
    EXPECT_FALSE(refusal("3.6b", argv)) << argv.front();
    EXPECT_FALSE(refusal("3.7", argv, false)) << "no daemon, nothing to crash";
  }
}

TEST(LiteralBreakPane, ANamedOneAndAWordTmuxRejectsAreNotRefused) {
  EXPECT_FALSE(refusal("3.7", {"break-pane", "-d", "-n", "w"}));
  EXPECT_FALSE(refusal("3.7", {"break-p", "-dnw"}));
  EXPECT_FALSE(refusal("3.7", {"split-window", ";", "list-panes"}));
  // Ambiguous and unknown words are rejected by tmux, with the whole list.
  EXPECT_FALSE(refusal("3.7", {"b", "-d"}));
  EXPECT_FALSE(refusal("3.7", {"breakpane", "-d"}));
  // A `-n` after `--` is an operand, not a flag.
  EXPECT_TRUE(refusal("3.7", {"break-pane", "--", "-n"}));
}

TEST(LiteralNamedBuffer, DeletionByNameIsRefusedBelow34InEverySpelling) {
  for (const Argv& argv :
       {Argv{"delete-buffer", "-b", "x"}, Argv{"deleteb", "-bx"},
        Argv{"delete-b", "-b", "x"},
        Argv{"set-buffer", "k", "v", ";", "delete-buffer", "-b", "x"},
        Argv{"split-window", "-d;", "deleteb", "-b", "x"}}) {
    const auto refused = refusal("3.3a", argv);
    ASSERT_TRUE(refused.has_value()) << argv.front();
    EXPECT_EQ(refused->kind, libtmux::FailureKind::unsupported);
    EXPECT_EQ(refused->delivery, libtmux::DeliveryStatus::not_started);
    EXPECT_NE(refused->diagnostic.find("deleting a named buffer needs tmux 3.4"),
              std::string::npos);
    EXPECT_FALSE(refusal("3.4", argv)) << argv.front();
    EXPECT_FALSE(refusal("3.8", argv)) << argv.front();
  }
}

TEST(LiteralNamedBuffer, TheTopBufferAndAnUnnamedDeletionAreLeftAlone) {
  EXPECT_FALSE(refusal("3.3a", {"delete-buffer"}));
  EXPECT_FALSE(refusal("3.3a", {"deleteb", "-t", "client"}));
  // Ambiguous: could be delete-buffer, detach-client ... for the `de` prefix.
  EXPECT_FALSE(refusal("3.3a", {"de", "-b", "x"}));
}

TEST(LiteralNamedBuffer, RenamingByNameIsRefusedBelow34) {
  for (const Argv& argv :
       {Argv{"set-buffer", "-b", "x", "-n", "y"}, Argv{"setb", "-nnew", "-bold"},
        Argv{"set-b", "-b", "x", "-n", "y"}}) {
    const auto refused = refusal("3.3a", argv);
    ASSERT_TRUE(refused.has_value()) << argv.front();
    EXPECT_EQ(refused->kind, libtmux::FailureKind::unsupported);
    EXPECT_NE(refused->diagnostic.find("renaming a named buffer needs tmux 3.4"),
              std::string::npos);
    EXPECT_FALSE(refusal("3.4", argv)) << argv.front();
  }
  // Creating or filling a named buffer, or renaming the top one, is safe.
  EXPECT_FALSE(refusal("3.3a", {"set-buffer", "-b", "x", "data"}));
  EXPECT_FALSE(refusal("3.3a", {"set-buffer", "-n", "y"}));
}

TEST(LiteralClipboardQuery, AQueryIsRefusedBelow36aInEverySpelling) {
  for (const Argv& argv :
       {Argv{"refresh-client", "-l"}, Argv{"refresh", "-t", "c", "-l"},
        Argv{"refresh-c", "-lt", "c"},
        Argv{"list-sessions", ";", "refresh-client", "-l"}}) {
    for (const char* tmux : {"3.3a", "3.4", "3.5", "3.5a", "3.6"}) {
      const auto failure = refusal(tmux, argv);
      ASSERT_TRUE(failure.has_value()) << argv.front() << " on " << tmux;
      EXPECT_EQ(failure->kind, libtmux::FailureKind::unsupported) << argv.front();
    }
    for (const char* tmux : {"3.6a", "3.6b", "3.8"}) {
      EXPECT_FALSE(refusal(tmux, argv)) << argv.front() << " on " << tmux;
    }
  }
}

TEST(LiteralClipboardQuery, ARefreshThatDoesNotQueryIsLeftAlone) {
  EXPECT_FALSE(refusal("3.3a", {"refresh-client", "-t", "c"}));
  EXPECT_FALSE(refusal("3.3a", {"refresh-client", "-f", "-l"}));
  EXPECT_FALSE(refusal("3.3a", {"display-message", "-l"}));
}

TEST(LiteralLayout, APresetThatNeedsANewerDaemonIsRefused) {
  for (const Argv& argv :
       {Argv{"select-layout", "main-vertical-mirrored"},
        Argv{"selectl", "-t", "%1", "--", "main-horizontal-mirrored"},
        Argv{"select-l", "-t", "%1", "main-horizontal-m"}}) {
    const auto refused = refusal("3.3a", argv);
    ASSERT_TRUE(refused.has_value()) << argv.front();
    EXPECT_EQ(refused->kind, libtmux::FailureKind::validation);
    EXPECT_EQ(refused->delivery, libtmux::DeliveryStatus::not_started);
    EXPECT_FALSE(refusal("3.5", argv)) << argv.front();
  }
}

TEST(LiteralLayout, ABadLayoutIsRefusedOnEveryRelease) {
  for (const char* tmux : {"3.3a", "3.5", "3.8"}) {
    EXPECT_TRUE(refusal(tmux, {"select-layout", "-t", "%1", "no-such-layout"})) << tmux;
    EXPECT_TRUE(refusal(tmux, {"selectl", "ffff,80x24,0,0,0"})) << tmux;
    EXPECT_FALSE(refusal(tmux, {"select-layout", "-t", "%1", "tiled"})) << tmux;
    EXPECT_FALSE(refusal(tmux, {"select-layout", "-t", "%1"})) << tmux;
    EXPECT_FALSE(refusal(tmux, {"select-layout", "-o"})) << tmux;
  }
}

TEST(LiteralLayout, AJsonLayoutNeedsTheReleaseThatReadsIt) {
  const std::string json = R"({"V":2,"L":{"t":"p","w":80,"h":24,"x":0,"y":0,"i":0}})";
  EXPECT_TRUE(refusal("3.7", {"select-layout", json}));
  EXPECT_FALSE(refusal("3.8-rc2", {"select-layout", json}));
}

TEST(LiteralNames, SeparatorsAreRefusedOnExactly37InEveryCommand) {
  for (const Argv& argv :
       {Argv{"new-session", "-d", "-s", "a.b"}, Argv{"new", "-ds", "a:b"},
        Argv{"new-session", "-n", "a.b"}, Argv{"new-window", "-n", "a:b"},
        Argv{"neww", "-nx.y"}, Argv{"rename-session", "-t", "s", "a.b"},
        Argv{"rename", "a:b"}, Argv{"rename-window", "a.b"},
        Argv{"renamew", "-t", "@1", "--", "x:y"},
        Argv{"list-panes", ";", "neww", "-n", "a.b"}}) {
    const auto refused = refusal("3.7", argv);
    ASSERT_TRUE(refused.has_value()) << argv.front();
    EXPECT_EQ(refused->kind, libtmux::FailureKind::validation);
    EXPECT_EQ(refused->delivery, libtmux::DeliveryStatus::not_started);
    EXPECT_NE(refused->diagnostic.find("':' or '.'"), std::string::npos);
    EXPECT_FALSE(refusal("3.7a", argv)) << argv.front();
    EXPECT_FALSE(refusal("3.6b", argv)) << argv.front();
  }
}

TEST(LiteralNames, APlainNameAndASeparatorElsewhereAreNotRefused) {
  EXPECT_FALSE(refusal("3.7", {"new-session", "-d", "-s", "plain"}));
  EXPECT_FALSE(refusal("3.7", {"new-window", "-t", "s:1", "-n", "plain"}));
  EXPECT_FALSE(refusal("3.7", {"rename-window", "-t", "s:1.0", "plain"}));
  EXPECT_FALSE(refusal("3.7", {"new-session", "-d", "-c", "/a.b"}));
}

} // namespace
