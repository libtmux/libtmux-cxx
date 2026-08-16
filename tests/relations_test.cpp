// Selecting an object by what its related objects look like.
//
// The quantifiers say what an empty relation means, which is the part that is
// easy to get silently wrong: a session with no windows satisfies `all_of` and
// `none_of` and fails `any_of`, exactly as the standard algorithms do.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "libtmux/cardinality.hpp"
#include "libtmux/entities.hpp"
#include "libtmux/relations.hpp"
#include "libtmux/server.hpp"
#include "libtmux/testing/scoped_server.hpp"

namespace {

using libtmux::all_of;
using libtmux::any_of;
using libtmux::children_of;
using libtmux::exactly_one;
using libtmux::is;
using libtmux::matching;
using libtmux::none_of;
using libtmux::parent_of;
using libtmux::Server;
using libtmux::Session;
using libtmux::Window;
namespace session = libtmux::session;
namespace window = libtmux::window;

Server connect(const libtmux::test::ScopedTmuxServer& fixture) {
  auto server = Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value());
  return server.value();
}

// Two sessions: one holding an editor, one that does not.
struct Listings {
  std::vector<Session> sessions;
  std::vector<Window> windows;
};

Listings arrange(const Server& server) {
  auto sessions = server.sessions();
  EXPECT_TRUE(sessions.has_value());
  EXPECT_FALSE(sessions->empty());
  const Session& first = sessions.value().at(0);
  EXPECT_TRUE(first.new_window("editor").has_value());
  EXPECT_TRUE(first.new_window("logs").has_value());

  const auto second = server.new_session("quiet");
  EXPECT_TRUE(second.has_value());
  EXPECT_TRUE(second->new_window("notes").has_value());

  auto listed_sessions = server.sessions();
  auto listed_windows = server.windows();
  EXPECT_TRUE(listed_sessions.has_value());
  EXPECT_TRUE(listed_windows.has_value());
  return Listings{std::move(listed_sessions).value(),
                  std::move(listed_windows).value()};
}

TEST(Relations, ASessionIsSelectedByTheWindowsItHolds) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const Listings listings = arrange(server);

  const auto windows_of =
      children_of<Session, Window>(listings.windows, window::session_id, session::id);

  auto editing =
      listings.sessions |
      matching(any_of<Session>("windows", windows_of, window::name == "editor"));
  const auto found = first(editing);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->get().window_count(), 3);

  auto quiet = listings.sessions | matching(none_of<Session>("windows", windows_of,
                                                             window::name == "editor"));
  const auto other = first(quiet);
  ASSERT_TRUE(other.has_value());
  EXPECT_EQ(other->get().name(), "quiet");
}

TEST(Relations, AWindowIsSelectedByTheSessionHoldingIt) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const Listings listings = arrange(server);

  const auto session_of =
      parent_of<Window, Session>(listings.sessions, window::session_id, session::id);
  const auto in_quiet = is<Window>("session", session_of, session::name == "quiet");

  // The quiet session holds the window new-session made and the one added
  // after it, so the relation alone selects both.
  auto quiet = listings.windows | matching(in_quiet);
  EXPECT_EQ(std::ranges::distance(quiet), 2);

  // A relation composes with a field test like any other expression.
  auto notes = listings.windows | matching(in_quiet && window::name == "notes");
  const auto only = exactly_one(notes);
  ASSERT_TRUE(only.has_value());
  EXPECT_EQ(only->get().name(), "notes");
}

TEST(Relations, AnEmptyRelationSatisfiesAllOfAndNotAnyOf) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const Listings listings = arrange(server);

  // A join against no rows at all is the empty relation for every session.
  const std::vector<Window> nothing;
  const auto no_windows =
      children_of<Session, Window>(nothing, window::session_id, session::id);

  auto vacuous =
      listings.sessions |
      matching(all_of<Session>("windows", no_windows, window::name == "editor"));
  auto none = listings.sessions | matching(any_of<Session>("windows", no_windows,
                                                           window::name == "editor"));

  EXPECT_EQ(std::ranges::distance(vacuous), std::ranges::distance(listings.sessions));
  EXPECT_EQ(std::ranges::distance(none), 0);
}

TEST(Relations, MatchingARelationNeverReachesTmux) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const Listings listings = arrange(server);

  // Nothing to reach: the listings were taken while it was running.
  ASSERT_TRUE(server.kill().has_value());
  ASSERT_FALSE(server.is_alive());

  const auto windows_of =
      children_of<Session, Window>(listings.windows, window::session_id, session::id);
  auto editing =
      listings.sessions |
      matching(any_of<Session>("windows", windows_of, window::name == "editor"));
  EXPECT_EQ(std::ranges::distance(editing), 1);
}

} // namespace
