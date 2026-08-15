// Binding a key, and taking the binding away.
//
// Nothing here reads `list-keys` into a structure: tmux prints a table name
// unquoted, so a listing holding `-T my table X command` cannot be told from
// the table `my` bound to the key `table`. The listing is asserted as the
// text tmux printed, and the library refuses to create a name that would
// make it ambiguous.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "libtmux/entities.hpp"
#include "libtmux/server.hpp"
#include "support/scoped_tmux_server.hpp"

namespace {

using libtmux::Server;

Server connect(const libtmux::test::ScopedTmuxServer& fixture) {
  auto server = Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value());
  return server.value();
}

// The listing tmux prints, as text. Structured reading of it is not offered.
std::string listing(const Server& server) {
  const auto printed = server.run({"list-keys"});
  EXPECT_TRUE(printed.has_value());
  return printed.value_or(std::string{});
}

TEST(KeyBindings, BindingMakesTheTableExistAndUnbindingTakesItAway) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  // A table exists only while something is bound in it, so unbinding in one
  // nothing has been bound in is refused. That refusal is what makes the
  // binding observable without parsing anything.
  const auto before = server.unbind_key("spiketable", "X");
  ASSERT_FALSE(before.has_value());
  EXPECT_EQ(before.error().kind, libtmux::FailureKind::refused);

  ASSERT_TRUE(
      server.bind_key("spiketable", "X", {"display-message", "bound"}).has_value());
  EXPECT_NE(listing(server).find("-T spiketable"), std::string::npos);

  ASSERT_TRUE(server.unbind_key("spiketable", "X").has_value());

  // Gone, and the table with it: the same call that worked now reports what
  // it did at the start.
  const auto after = server.unbind_key("spiketable", "X");
  ASSERT_FALSE(after.has_value());
  EXPECT_NE(after.error().diagnostic.find("spiketable"), std::string::npos);
  EXPECT_EQ(listing(server).find("-T spiketable"), std::string::npos);
}

TEST(KeyBindings, ATableNameThatCouldNotBeReadBackIsRefused) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  // tmux accepts this and prints it unquoted, at which point its own listing
  // is ambiguous. Refused here rather than left to be discovered later.
  const auto spaced = server.bind_key("my table", "X", {"display-message", "x"});
  ASSERT_FALSE(spaced.has_value());
  EXPECT_EQ(spaced.error().kind, libtmux::FailureKind::validation);
  EXPECT_FALSE(spaced.error().dispatched);
  EXPECT_EQ(listing(server).find("my table"), std::string::npos);

  for (const auto& refused :
       {server.bind_key("", "X", {"display-message", "x"}),
        server.bind_key("tbl", "X", {}), server.unbind_key("my table", "X")}) {
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().kind, libtmux::FailureKind::validation);
    EXPECT_FALSE(refused.error().dispatched);
  }

  // An empty key is refused by tmux rather than here: it says `unknown key`
  // and creates nothing, so repeating the check would only move the same
  // answer a round trip earlier.
  const auto blank = server.bind_key("tbl", "", {"display-message", "x"});
  ASSERT_FALSE(blank.has_value());
  EXPECT_EQ(blank.error().kind, libtmux::FailureKind::refused);
  EXPECT_TRUE(blank.error().dispatched);
}

TEST(KeyBindings, AnUnknownKeyIsTmuxsToRefuse) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  // Unlike `send-keys`, which accepts a name it does not know and silently
  // sends nothing, `bind-key` says so — so nothing here duplicates tmux's
  // vocabulary.
  const auto refused =
      server.bind_key("spiketable", "No Such Key", {"display-message", "never"});
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().kind, libtmux::FailureKind::refused);
  EXPECT_TRUE(refused.error().dispatched);
  EXPECT_NE(refused.error().diagnostic.find("No Such Key"), std::string::npos);
}

TEST(KeyBindings, TheCommandIsArgvSoNothingNeedsQuoting) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  // An argument with a space in it stays one argument. tmux prints it back
  // quoted, which is how the test can tell it did not become two.
  ASSERT_TRUE(
      server.bind_key("spiketable", "A", {"display-message", "two words"}).has_value());

  const std::string printed = listing(server);
  EXPECT_NE(printed.find("display-message \"two words\""), std::string::npos)
      << printed;
}

TEST(KeyBindings, UnbindingAKeyThatWasNotBoundLeavesTheRest) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  ASSERT_TRUE(
      server.bind_key("spiketable", "A", {"display-message", "kept"}).has_value());

  // tmux treats this as nothing to do rather than an error, and the binding
  // that is there is untouched.
  EXPECT_TRUE(server.unbind_key("spiketable", "Q").has_value());

  EXPECT_NE(listing(server).find("-T spiketable"), std::string::npos);
}

TEST(KeyBindings, ARepeatableBindingSaysSoInTheListing) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  ASSERT_TRUE(server
                  .bind_key("spiketable", "B", {"display-message", "again"},
                            /*repeatable=*/true)
                  .has_value());

  // tmux prints the flag it was given, which is the only place the
  // difference between the two calls shows up.
  const std::string printed = listing(server);
  EXPECT_NE(printed.find("bind-key -r -T spiketable"), std::string::npos) << printed;
}

} // namespace
