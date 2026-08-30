// Taking the pane tree apart and putting it back.

#include <algorithm>

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

TEST(PaneTopology, JoinMovesAPaneAndEmptiesTheWindowItLeft) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;
  const auto home = session->active_window();
  ASSERT_TRUE(home.has_value()) << home.error().diagnostic;

  // A pane in a window of its own, made by the half that already existed.
  const auto split = home->split();
  ASSERT_TRUE(split.has_value()) << split.error().diagnostic;
  const auto moved_out = split->break_out();
  ASSERT_TRUE(moved_out.has_value()) << moved_out.error().diagnostic;
  ASSERT_NE(moved_out->id(), home->id());
  const auto before = session->windows();
  ASSERT_TRUE(before.has_value()) << before.error().diagnostic;
  ASSERT_EQ(before->size(), 2U);

  ASSERT_TRUE(split->join(*home).has_value());

  // The pane is back, keeping its id, and the window it vacated is gone
  // because it held nothing else.
  const auto after = session->windows();
  ASSERT_TRUE(after.has_value()) << after.error().diagnostic;
  EXPECT_EQ(after->size(), 1U);
  const auto panes = home->panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  ASSERT_EQ(panes->size(), 2U);
  EXPECT_TRUE(std::ranges::any_of(
      *panes, [&](const libtmux::Pane& one) { return one.id() == split->id(); }));
}

TEST(PaneTopology, ATitleIsSetAndSurvivesTheProcessBeingReplaced) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;
  const auto pane = session->active_pane();
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;

  // A leading dash, because a title is data and would otherwise be read as
  // a flag of select-pane.
  ASSERT_TRUE(pane->set_title("-titled").has_value());
  const auto named = pane->refresh();
  ASSERT_TRUE(named.has_value()) << named.error().diagnostic;
  EXPECT_EQ(named->title(), "-titled");

  ASSERT_TRUE(pane->respawn(/*replace_running=*/true).has_value());
  const auto after = pane->refresh();
  ASSERT_TRUE(after.has_value()) << after.error().diagnostic;
  EXPECT_EQ(after->title(), "-titled")
      << "the title belongs to the pane, not its process";
}

TEST(PaneTopology, RespawnRefusesALivePaneUnlessToldToReplaceIt) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;
  const auto pane = session->active_pane();
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;

  // tmux refuses rather than killing something that is still running, and
  // the refusal is worth keeping: replacing a live process is a decision.
  const auto refused = pane->respawn();
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().kind, libtmux::FailureKind::refused);
  EXPECT_FALSE(refused.error().diagnostic.empty());

  EXPECT_TRUE(pane->respawn(/*replace_running=*/true).has_value());
}

TEST(PaneTopology, PipingCopiesOutputUntilItIsStopped) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;
  const auto pane = session->active_pane();
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;
  EXPECT_FALSE(pane->piping());

  // Asserted through tmux's own view rather than the file the command
  // writes: whether the pipe is open is state tmux reports, while the file
  // depends on a shell being scheduled.
  ASSERT_TRUE(pane->pipe_to("cat > /dev/null").has_value());
  const auto piping = pane->refresh();
  ASSERT_TRUE(piping.has_value()) << piping.error().diagnostic;
  EXPECT_TRUE(piping->piping());

  ASSERT_TRUE(pane->stop_piping().has_value());
  const auto stopped = pane->refresh();
  ASSERT_TRUE(stopped.has_value()) << stopped.error().diagnostic;
  EXPECT_FALSE(stopped->piping());

  // Stopping a pane that was not piping is harmless.
  EXPECT_TRUE(pane->stop_piping().has_value());
}

TEST(PaneTopology, PipingRefusesAnEmptyCommandRatherThanStopping) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;
  const auto pane = session->active_pane();
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;
  ASSERT_TRUE(pane->pipe_to("cat > /dev/null").has_value());

  // tmux spells "stop" as an empty command. A caller who asked to pipe did
  // not mean to stop, so this refuses rather than doing the opposite.
  const auto refused = pane->pipe_to("");
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().kind, libtmux::FailureKind::validation);
  EXPECT_EQ(refused.error().delivery, libtmux::DeliveryStatus::not_started);

  const auto still = pane->refresh();
  ASSERT_TRUE(still.has_value()) << still.error().diagnostic;
  EXPECT_TRUE(still->piping()) << "the refusal must not have stopped the pipe";
}

} // namespace
