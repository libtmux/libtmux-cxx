// Choosing which window is active.
//
// Behavior evidence for the window-layout shard of the parity ledger, one
// case per classified capability.

#include <string>
#include <string_view>
#include <vector>

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

TEST(WindowLayout, SelectMakesOneWindowTheActiveOne) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;

  libtmux::NewWindowOptions options;
  options.name = "second";
  ASSERT_TRUE(session->new_window(options).has_value());
  const auto windows = session->windows();
  ASSERT_TRUE(windows.has_value()) << windows.error().diagnostic;
  ASSERT_EQ(windows->size(), 2U);

  ASSERT_TRUE(windows->front().select().has_value());

  // Asked again, because an entity reads the moment it was listed: the
  // value in hand still says what was true before the selection moved.
  const auto active = session->active_window();
  ASSERT_TRUE(active.has_value()) << active.error().diagnostic;
  EXPECT_EQ(active->id(), windows->front().id());
  EXPECT_FALSE(windows->back().refresh().value().active());
}

// Four panes under a named layout, so a change in arrangement is visible.
std::vector<libtmux::Pane> panes_under(const libtmux::Session& session,
                                       std::string_view layout) {
  const auto window = session.active_window();
  EXPECT_TRUE(window.has_value());
  for (int made = 0; made < 3; ++made) {
    EXPECT_TRUE(window->split().has_value());
  }
  EXPECT_TRUE(window->select_layout(layout).has_value());
  auto panes = window->panes();
  EXPECT_TRUE(panes.has_value());
  return panes.has_value() ? *panes : std::vector<libtmux::Pane>{};
}

std::vector<std::string> geometry(const libtmux::Window& window) {
  std::vector<std::string> cells;
  const auto panes = window.panes();
  EXPECT_TRUE(panes.has_value());
  if (panes.has_value()) {
    for (const libtmux::Pane& pane : *panes) {
      cells.push_back(std::to_string(pane.width()) + "x" +
                      std::to_string(pane.height()));
    }
  }
  return cells;
}

TEST(WindowLayout, NextAndPreviousStepThroughTmuxsArrangements) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;
  // From "tiled", tmux's next arrangement seats the panes differently. From
  // a layout whose cells are already degenerate at this window size, it does
  // not, and the test would be asserting nothing.
  ASSERT_EQ(panes_under(*session, "tiled").size(), 4U);
  auto window = session->active_window();
  ASSERT_TRUE(window.has_value()) << window.error().diagnostic;

  const auto named = geometry(*window);
  ASSERT_TRUE(window->next_layout().has_value());
  const auto stepped = geometry(*window);
  EXPECT_NE(stepped, named) << "the next arrangement should sit the panes differently";

  ASSERT_TRUE(window->previous_layout().has_value());
  EXPECT_EQ(geometry(*window), named);
}

TEST(WindowLayout, RotateMovesThePanesAndLeavesTheCells) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;
  const auto before = panes_under(*session, "main-vertical");
  ASSERT_EQ(before.size(), 4U);
  auto window = session->active_window();
  ASSERT_TRUE(window.has_value()) << window.error().diagnostic;
  const auto cells = geometry(*window);

  ASSERT_TRUE(window->rotate().has_value());

  // The cells are where they were; a different pane occupies the first one.
  EXPECT_EQ(geometry(*window), cells);
  const auto after = window->panes();
  ASSERT_TRUE(after.has_value()) << after.error().diagnostic;
  ASSERT_EQ(after->size(), before.size());
  EXPECT_NE(after->front().id(), before.front().id())
      << "rotating should put another pane in the first cell";
}

TEST(WindowLayout, LastPaneReturnsToThePreviouslySelectedOne) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;
  const auto panes = panes_under(*session, "tiled");
  ASSERT_EQ(panes.size(), 4U);
  const auto window = session->active_window();
  ASSERT_TRUE(window.has_value()) << window.error().diagnostic;

  // Non-adjacent on purpose, so "last" cannot be confused with a step to
  // the neighbouring pane.
  ASSERT_TRUE(panes.front().select().has_value());
  ASSERT_TRUE(panes[2].select().has_value());

  const auto back = window->select_last_pane();
  ASSERT_TRUE(back.has_value()) << back.error().diagnostic;
  EXPECT_EQ(back->id(), panes.front().id());
}

TEST(WindowLayout, LastPaneRefusesWhenThereIsOnlyOne) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;
  const auto window = session->active_window();
  ASSERT_TRUE(window.has_value()) << window.error().diagnostic;
  const auto panes = window->panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  ASSERT_EQ(panes->size(), 1U);

  const auto refused = window->select_last_pane();
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().kind, libtmux::FailureKind::refused);
  EXPECT_FALSE(refused.error().diagnostic.empty());
}

TEST(WindowIndex, AWindowIsPutWhereItIsAskedFor) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;

  // Not the next free one: a workspace description names the index it wants,
  // and 7 is far enough past the end that landing there cannot be luck.
  libtmux::NewWindowOptions options;
  options.name = "placed";
  options.index = 7;
  const auto placed = session->new_window(options);
  ASSERT_TRUE(placed.has_value()) << placed.error().diagnostic;
  EXPECT_EQ(placed->index(), 7);

  // An index already in use is refused rather than quietly moved: tmux says
  // so, and a workspace rebuilt over a running one should hear it.
  const auto again = session->new_window(options);
  ASSERT_FALSE(again.has_value());
  EXPECT_TRUE(again.error().dispatched);

  // Without one, tmux picks, and the earlier window is still where it was.
  libtmux::NewWindowOptions unplaced;
  unplaced.name = "wherever";
  const auto anywhere = session->new_window(unplaced);
  ASSERT_TRUE(anywhere.has_value()) << anywhere.error().diagnostic;
  EXPECT_NE(anywhere->index(), 7);
  const auto refreshed = placed->refresh();
  ASSERT_TRUE(refreshed.has_value()) << refreshed.error().diagnostic;
  EXPECT_EQ(refreshed->index(), 7);
}

} // namespace
