// Making a session, and ending the server that holds it.
//
// Behavior evidence for the server-sessions shard of the parity ledger. One
// case per classified capability, because the ledger binds one evidence
// record to one CTest test.

#include <string>

#include <chrono>

#include <gtest/gtest.h>

#include "libtmux/entities.hpp"
#include "libtmux/server.hpp"
#include "libtmux/testing/scoped_server.hpp"

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

// Fields tmux answers for every session and window, which nothing read.
//
// `libtmux.compatibility` proves tmux 3.2a knows each of these format tokens.
// That is a different claim from the value decoding correctly, and these are
// the four where nothing made the second one.

TEST(SessionFields, ReportsWhenTheSessionWasCreated) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto before = std::chrono::floor<std::chrono::seconds>(
      std::chrono::system_clock::now() - std::chrono::minutes{5});

  const Server server = connect(*fixture);
  const auto sessions = server.sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  ASSERT_FALSE(sessions->empty());

  // A timestamp rather than a number that happens to parse: it has to sit
  // between a moment before the fixture started and a moment after.
  const auto created = sessions->at(0).created();
  const auto after = std::chrono::floor<std::chrono::seconds>(
      std::chrono::system_clock::now() + std::chrono::minutes{5});
  EXPECT_GT(created, before);
  EXPECT_LT(created, after);
}

TEST(SessionFields, ReportsGroupMembership) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto sessions = server.sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  ASSERT_FALSE(sessions->empty());
  // A session made on its own belongs to no group, and says so rather than
  // leaving the reader to interpret an empty name.
  EXPECT_FALSE(sessions->at(0).grouped());
  EXPECT_TRUE(sessions->at(0).group().empty());

  // Through `run`, because `NewSessionOptions` has no way to say "grouped
  // with": the typed surface cannot create the state these two fields report.
  const auto grouped = server.run({"new-session", "-d", "-s", "in-a-group", "-t",
                                   std::string{fixture->session_name()}});
  ASSERT_TRUE(grouped.has_value()) << grouped.error().diagnostic;

  const auto after = server.sessions();
  ASSERT_TRUE(after.has_value()) << after.error().diagnostic;
  bool saw_group = false;
  for (const libtmux::Session& session : *after) {
    if (session.grouped()) {
      saw_group = true;
      EXPECT_FALSE(session.group().empty()) << session.name();
    }
  }
  EXPECT_TRUE(saw_group) << "a grouped session reported no group";
}

TEST(WindowFields, ReportsWhetherAWindowIsZoomed) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto windows = server.windows();
  ASSERT_TRUE(windows.has_value()) << windows.error().diagnostic;
  ASSERT_FALSE(windows->empty());
  const std::string id{windows->at(0).id()};
  EXPECT_FALSE(windows->at(0).zoomed()) << "a fresh window reported zoomed";

  // Zooming needs something to zoom into, so split first.
  const auto split = server.run({"split-window", "-d", "-t", id});
  ASSERT_TRUE(split.has_value()) << split.error().diagnostic;

  const auto zoomed = server.run({"resize-pane", "-Z", "-t", id});
  ASSERT_TRUE(zoomed.has_value()) << zoomed.error().diagnostic;

  const auto after = server.window(id);
  ASSERT_TRUE(after.has_value()) << after.error().diagnostic;
  EXPECT_TRUE(after->zoomed()) << "zoomed a window and it said otherwise";
}
