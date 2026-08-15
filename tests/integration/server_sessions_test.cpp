// Making a session, and ending the server that holds it.
//
// Behavior evidence for the server-sessions shard of the parity ledger. One
// case per classified capability, because the ledger binds one evidence
// record to one CTest test.

#include <string>

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

TEST(ServerSessions, NewSessionCreatesOneAndHandsItBack) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto created = server.new_session("made");
  ASSERT_TRUE(created.has_value()) << created.error().diagnostic;
  EXPECT_EQ(created->name(), "made");

  // The server agrees it exists, and the returned value names the same one.
  const auto found = server.session("made");
  ASSERT_TRUE(found.has_value()) << found.error().diagnostic;
  EXPECT_EQ(found->id(), created->id());

  // Detached, so a library call never takes the terminal.
  EXPECT_FALSE(created->attached());
}

TEST(ServerSessions, KillEndsTheServerAndEverythingOnIt) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  ASSERT_TRUE(server.new_session("doomed").has_value());
  ASSERT_TRUE(server.is_alive());

  ASSERT_TRUE(server.kill().has_value());

  // Not "the sessions are gone" — the server itself is, which is why the
  // sessions are. Listing a dead server answers empty rather than failing.
  EXPECT_FALSE(server.is_alive());
  const auto listed = server.sessions();
  EXPECT_TRUE(!listed.has_value() || listed->empty());
}

} // namespace
