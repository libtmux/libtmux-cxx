// Options and hooks, at the scope tmux keeps them in.
//
// tmux has four option scopes and one command that reads all of them, told
// apart only by a flag. Getting the flag wrong reads or writes the wrong
// scope silently, which is why each scope is exercised against a real server.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "libtmux/entities.hpp"
#include "libtmux/options.hpp"
#include "libtmux/server.hpp"
#include "support/scoped_tmux_server.hpp"

namespace {

using libtmux::FailureKind;
using libtmux::OptionEntry;
using libtmux::parse_option;
using libtmux::Server;
using libtmux::Session;

Server connect(const libtmux::test::ScopedTmuxServer& fixture) {
  auto server = Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value());
  return server.value();
}

Session only_session(const Server& server) {
  auto sessions = server.sessions();
  EXPECT_TRUE(sessions.has_value());
  EXPECT_EQ(sessions->size(), 1U);
  return sessions.value().at(0);
}

TEST(Options, AnInheritedOptionKeepsItsNameAndSaysItIsInherited) {
  // `show-options -A` marks an option that comes from a parent scope with a
  // trailing asterisk on the name, which is not part of the name.
  const auto inherited = parse_option("status-position* bottom");
  ASSERT_TRUE(inherited.has_value());
  EXPECT_EQ(inherited->name, "status-position");
  EXPECT_EQ(inherited->value, "bottom");
  EXPECT_TRUE(inherited->inherited);

  const auto own = parse_option("status-position top");
  ASSERT_TRUE(own.has_value());
  EXPECT_EQ(own->name, "status-position");
  EXPECT_FALSE(own->inherited);
}

TEST(Options, AValueSurvivesBeingReadAndWrittenBack) {
  // tmux picks one of four quoting forms depending on what is in the value,
  // and a reader that knows only one corrupts the rest — which matters because
  // reading an option and writing it back is what a workspace tool does.
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const Session session = only_session(server);

  const std::vector<std::string> awkward{
      "plain",      "has space",
      "a\"b",       "a'b",
      "a#b",        "~/x",
      "a\\b",       "",
      "tab\there",  "new\nline",
      "semi;colon", "dollar${x}",
      "brace{}",    "quote\"and'both",
  };
  for (const std::string& value : awkward) {
    ASSERT_TRUE(session.set_option("@probe", value).has_value()) << value;
    const auto read = session.option("@probe");
    ASSERT_TRUE(read.has_value()) << read.error().diagnostic;
    EXPECT_EQ(read->value, value) << "round trip changed the value";

    // And writing back what was read leaves it unchanged.
    ASSERT_TRUE(session.set_option("@probe", read->value).has_value());
    const auto again = session.option("@probe");
    ASSERT_TRUE(again.has_value()) << again.error().diagnostic;
    EXPECT_EQ(again->value, value) << "writing back what was read changed it";
  }
}

TEST(Options, AnOptionNameThatEndsInTheMarkerIsStillItsName) {
  // tmux accepts a user option whose name ends in an asterisk, and prints one
  // set here as `@star* value` — the same shape it uses to mark an inherited
  // option. Asking by name settles which it is; a fuzzer found this.
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const Session session = only_session(server);

  ASSERT_TRUE(session.set_option("@star*", "value").has_value());
  const auto read = session.option("@star*");
  ASSERT_TRUE(read.has_value()) << read.error().diagnostic;
  EXPECT_EQ(read->name, "@star*") << "the name lost a character";
  EXPECT_EQ(read->value, "value");
  EXPECT_FALSE(read->inherited) << "a name ending in the marker read as inherited";

  // And an ordinary inherited option is still reported as inherited.
  const auto inherited = session.option("status-position");
  ASSERT_TRUE(inherited.has_value()) << inherited.error().diagnostic;
  EXPECT_TRUE(inherited->inherited);
}

TEST(Options, ASessionOptionIsWrittenAndReadBackAtItsOwnScope) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const Session session = only_session(server);

  ASSERT_TRUE(session.set_option("status-position", "top").has_value());

  const auto read = session.option("status-position");
  ASSERT_TRUE(read.has_value()) << read.error().diagnostic;
  EXPECT_EQ(read->value, "top");
  EXPECT_FALSE(read->inherited);

  ASSERT_TRUE(session.unset_option("status-position").has_value());
  const auto restored = session.option("status-position");
  ASSERT_TRUE(restored.has_value()) << restored.error().diagnostic;
  EXPECT_TRUE(restored->inherited);
}

TEST(Options, AnOptionResolvesThroughTheTargetsScopeChain) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const Session session = only_session(server);

  const auto window = session.new_window("scoped");
  ASSERT_TRUE(window.has_value()) << window.error().diagnostic;
  const auto panes = window->panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  ASSERT_FALSE(panes->empty());

  ASSERT_TRUE(window->set_option("main-pane-width", "101").has_value());
  const auto width = window->option("main-pane-width");
  ASSERT_TRUE(width.has_value()) << width.error().diagnostic;
  EXPECT_EQ(width->value, "101");
  EXPECT_FALSE(width->inherited);

  ASSERT_TRUE(panes->front().set_option("remain-on-exit", "on").has_value());
  const auto remain = panes->front().option("remain-on-exit");
  ASSERT_TRUE(remain.has_value()) << remain.error().diagnostic;
  EXPECT_EQ(remain->value, "on");

  // A session option is visible from a pane, because a pane inherits from its
  // window and a window from its session. It is reported as inherited, which
  // is how a caller tells a value that is set here from one that is not.
  const auto position = panes->front().option("status-position");
  ASSERT_TRUE(position.has_value()) << position.error().diagnostic;
  EXPECT_TRUE(position->inherited);
}

TEST(Options, AnOptionTmuxDoesNotKnowIsRefusedByName) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const Session session = only_session(server);

  const auto written = session.set_option("no-such-option", "x");
  ASSERT_FALSE(written.has_value());
  EXPECT_NE(written.error().diagnostic.find("no-such-option"), std::string::npos);
}

TEST(Options, EveryScopeListsSomething) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const Session session = only_session(server);
  const auto window = session.active_window();
  ASSERT_TRUE(window.has_value()) << window.error().diagnostic;

  const auto server_options = server.server_options();
  ASSERT_TRUE(server_options.has_value()) << server_options.error().diagnostic;
  EXPECT_FALSE(server_options->empty());

  const auto session_options = session.options();
  ASSERT_TRUE(session_options.has_value()) << session_options.error().diagnostic;
  EXPECT_FALSE(session_options->empty());

  const auto window_options = window->options();
  ASSERT_TRUE(window_options.has_value()) << window_options.error().diagnostic;
  EXPECT_FALSE(window_options->empty());
}

TEST(Options, AHookIsSetOnASessionAndReadBack) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const Session session = only_session(server);

  ASSERT_TRUE(
      session.set_hook("after-new-window", "display-message hooked").has_value());

  const auto hooks = session.hooks();
  ASSERT_TRUE(hooks.has_value()) << hooks.error().diagnostic;
  bool found = false;
  for (const OptionEntry& hook : *hooks) {
    if (hook.name == "after-new-window") {
      found = true;
      EXPECT_EQ(hook.value, "display-message hooked");
    }
  }
  EXPECT_TRUE(found) << "the hook that was just set was not listed";
}

} // namespace
