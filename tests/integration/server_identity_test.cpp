// Which tmux server a value came from, and what depends on knowing.
//
// tmux numbers ids per server. Two servers started a second apart both hold
// `$0`, `@0` and `%0`, so an id carried from one to the other does not fail to
// resolve — it resolves to something else, and tmux reports success. Every
// assertion here rests on that: the fixtures deliberately collide.

#include <string>
#include <unordered_set>

#include <gtest/gtest.h>

#include "libtmux/cardinality.hpp"
#include "libtmux/entities.hpp"
#include "libtmux/server.hpp"
#include "support/environment_guard.hpp"
#include "support/scoped_tmux_server.hpp"

namespace {

using libtmux::Server;

Server connect(const libtmux::test::ScopedTmuxServer& fixture) {
  auto server = Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value());
  return server.value();
}

// The two servers really do use the same ids, which is what makes every
// refusal below load-bearing rather than theoretical.
TEST(ServerIdentity, TwoServersNumberTheirObjectsTheSameWay) {
  auto left = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(left.has_value()) << left.error();
  auto right = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(right.has_value()) << right.error();

  const auto here = connect(*left).panes();
  ASSERT_TRUE(here.has_value()) << here.error().diagnostic;
  const auto there = connect(*right).panes();
  ASSERT_TRUE(there.has_value()) << there.error().diagnostic;
  ASSERT_FALSE(here->empty());
  ASSERT_FALSE(there->empty());

  EXPECT_EQ(here->front().id(), there->front().id());
  // Same id, different servers, so not the same pane.
  EXPECT_NE(here->front(), there->front());
}

TEST(ServerIdentity, SwappingPanesAcrossServersIsRefusedRatherThanMisdirected) {
  auto left = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(left.has_value()) << left.error();
  auto right = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(right.has_value()) << right.error();

  const Server here = connect(*left);
  const Server there = connect(*right);
  const auto mine = here.panes();
  ASSERT_TRUE(mine.has_value()) << mine.error().diagnostic;
  const auto theirs = there.panes();
  ASSERT_TRUE(theirs.has_value()) << theirs.error().diagnostic;

  // Without the check this ran against `left` carrying `right`'s pane id,
  // found the pane of that id on `left`, swapped it, and answered success.
  const auto refused = mine->front().swap_with(theirs->front());
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().kind, libtmux::FailureKind::validation);
  EXPECT_FALSE(refused.error().dispatched)
      << "a refusal before dispatch is what makes it safe to report";
  EXPECT_NE(refused.error().diagnostic.find("different tmux servers"),
            std::string::npos)
      << "diagnostic was: " << refused.error().diagnostic;
}

TEST(ServerIdentity, EveryCommandCombiningTwoValuesChecksTheirServers) {
  auto left = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(left.has_value()) << left.error();
  auto right = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(right.has_value()) << right.error();

  const Server here = connect(*left);
  const Server there = connect(*right);

  const auto my_sessions = here.sessions();
  ASSERT_TRUE(my_sessions.has_value()) << my_sessions.error().diagnostic;
  const auto their_sessions = there.sessions();
  ASSERT_TRUE(their_sessions.has_value()) << their_sessions.error().diagnostic;
  const auto my_windows = here.windows();
  ASSERT_TRUE(my_windows.has_value()) << my_windows.error().diagnostic;
  const auto their_windows = there.windows();
  ASSERT_TRUE(their_windows.has_value()) << their_windows.error().diagnostic;
  const auto my_panes = here.panes();
  ASSERT_TRUE(my_panes.has_value()) << my_panes.error().diagnostic;
  const auto their_panes = there.panes();
  ASSERT_TRUE(their_panes.has_value()) << their_panes.error().diagnostic;

  const auto refuses = [](const auto& outcome, std::string_view what) {
    ASSERT_FALSE(outcome.has_value()) << what << " should have been refused";
    EXPECT_EQ(outcome.error().kind, libtmux::FailureKind::validation) << what;
    EXPECT_FALSE(outcome.error().dispatched) << what;
  };

  refuses(my_windows->front().link_to(their_sessions->front()), "link_to");
  refuses(my_windows->front().swap_with(their_windows->front()), "window swap_with");
  refuses(my_panes->front().swap_with(their_panes->front()), "pane swap_with");
  refuses(my_panes->front().join(their_windows->front()), "join");

  ASSERT_TRUE(there.set_buffer("crossed", "content").has_value());
  const auto their_buffers = there.buffers();
  ASSERT_TRUE(their_buffers.has_value()) << their_buffers.error().diagnostic;
  ASSERT_FALSE(their_buffers->empty());
  refuses(my_panes->front().paste(their_buffers->front()), "paste");
}

// The same server reached twice is one server. Two `Server` values over one
// socket used to produce entities that compared unequal, because equality was
// which handle they came through rather than which tmux they name.
TEST(ServerIdentity, TwoHandlesOnOneSocketDescribeTheSameObjects) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();

  const Server first = connect(*fixture);
  const Server second = connect(*fixture);
  ASSERT_NE(&first, &second);

  const auto here = first.sessions();
  ASSERT_TRUE(here.has_value()) << here.error().diagnostic;
  const auto there = second.sessions();
  ASSERT_TRUE(there.has_value()) << there.error().diagnostic;
  ASSERT_FALSE(here->empty());
  ASSERT_FALSE(there->empty());

  EXPECT_EQ(here->front(), there->front());
  // Equal values hash alike, so one keys a container the other can look in.
  std::unordered_set<libtmux::Session> seen;
  seen.insert(here->front());
  EXPECT_EQ(seen.count(there->front()), 1U);

  // And a command combining them is allowed, because it is one server.
  const auto windows = first.windows();
  ASSERT_TRUE(windows.has_value()) << windows.error().diagnostic;
  const auto other = second.new_session("elsewhere");
  ASSERT_TRUE(other.has_value()) << other.error().diagnostic;
  EXPECT_TRUE(windows->front().link_to(*other).has_value());
}

// A path and the name that resolves to it select one server, so values from
// the two spellings are the same values. Comparing the argv instead called
// them two.
TEST(ServerIdentity, ANameAndThePathItResolvesToAreOneServer) {
  libtmux::test::ScopedTmuxServerOptions options;
  options.mode = libtmux::test::SocketMode::Name;
  auto fixture = libtmux::test::ScopedTmuxServer::start(std::move(options));
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto name = fixture->socket_name();
  ASSERT_TRUE(name.has_value());

  const libtmux::test::EnvironmentGuard tmpdir{"TMUX_TMPDIR",
                                               fixture->tmux_tmpdir().string()};
  const auto by_name = Server::at_socket_name(std::string{*name});
  ASSERT_TRUE(by_name.has_value());

  // Ask tmux where that name landed, then reach the same server by that path.
  const auto socket = by_name->expand("#{socket_path}");
  ASSERT_TRUE(socket.has_value()) << socket.error().diagnostic;
  const auto by_path = Server::at_socket_path(*socket);
  ASSERT_TRUE(by_path.has_value());

  const auto named = by_name->sessions();
  ASSERT_TRUE(named.has_value()) << named.error().diagnostic;
  const auto pathed = by_path->sessions();
  ASSERT_TRUE(pathed.has_value()) << pathed.error().diagnostic;
  ASSERT_FALSE(named->empty());
  ASSERT_FALSE(pathed->empty());

  EXPECT_EQ(named->front(), pathed->front())
      << "the same tmux reached two ways is the same tmux";
}

// The resolution here is a copy of tmux's, so tmux is the oracle: if a release
// changes where a label lands, this fails rather than the library quietly
// deciding two servers are one.
TEST(ServerIdentity, ResolutionAgreesWithTheRunningTmux) {
  libtmux::test::ScopedTmuxServerOptions options;
  options.mode = libtmux::test::SocketMode::Name;
  auto fixture = libtmux::test::ScopedTmuxServer::start(std::move(options));
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto name = fixture->socket_name();
  ASSERT_TRUE(name.has_value());

  const libtmux::test::EnvironmentGuard tmpdir{"TMUX_TMPDIR",
                                               fixture->tmux_tmpdir().string()};
  const auto server = Server::at_socket_name(std::string{*name});
  ASSERT_TRUE(server.has_value());

  const auto reported = server->expand("#{socket_path}");
  ASSERT_TRUE(reported.has_value()) << reported.error().diagnostic;

  // A path-selected server over the same socket must identify as the same
  // string the name-selected one resolved to, which is only true when the
  // resolution matched what tmux did.
  const auto twin = Server::at_socket_path(*reported);
  ASSERT_TRUE(twin.has_value());
  const auto sessions = server->sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  const auto twin_sessions = twin->sessions();
  ASSERT_TRUE(twin_sessions.has_value()) << twin_sessions.error().diagnostic;
  ASSERT_FALSE(sessions->empty());
  ASSERT_FALSE(twin_sessions->empty());
  EXPECT_EQ(sessions->front().connection_identity(),
            twin_sessions->front().connection_identity())
      << "resolved to " << sessions->front().connection_identity() << ", tmux says "
      << *reported;
}

} // namespace
