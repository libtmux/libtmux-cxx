// Moving the window selection, against a real server.
//
// Behavior evidence for the session-navigation shard of the parity ledger,
// which is why it lives here rather than beside the other entity tests: the
// contract requires a behavior artifact under this directory.

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

// Four windows, so "last" and "next" cannot be confused: from the third,
// next lands on the fourth while last returns to the first.
std::vector<libtmux::Window> four_windows(const libtmux::Session& session) {
  for (const auto* name : {"two", "three", "four"}) {
    libtmux::NewWindowOptions options;
    options.name = name;
    EXPECT_TRUE(session.new_window(options).has_value());
  }
  auto windows = session.windows();
  EXPECT_TRUE(windows.has_value());
  return windows.has_value() ? *windows : std::vector<libtmux::Window>{};
}

libtmux::Session open(const libtmux::test::ScopedTmuxServer& fixture,
                      const Server& server) {
  auto session = server.session(fixture.session_name());
  EXPECT_TRUE(session.has_value());
  return session.value();
}

TEST(SessionNavigation, NextWindowAdvancesAndWrapsAtTheEnd) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const auto session = open(*fixture, server);
  const auto windows = four_windows(session);
  ASSERT_EQ(windows.size(), 4U);
  ASSERT_TRUE(windows.front().select().has_value());

  const auto next = session.select_next_window();
  ASSERT_TRUE(next.has_value()) << next.error().diagnostic;
  EXPECT_EQ(next->id(), windows[1].id());

  // From the last window it wraps rather than stopping.
  ASSERT_TRUE(windows.back().select().has_value());
  const auto wrapped = session.select_next_window();
  ASSERT_TRUE(wrapped.has_value()) << wrapped.error().diagnostic;
  EXPECT_EQ(wrapped->id(), windows.front().id());
}

TEST(SessionNavigation, PreviousWindowStepsBack) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const auto session = open(*fixture, server);
  const auto windows = four_windows(session);
  ASSERT_EQ(windows.size(), 4U);
  ASSERT_TRUE(windows[1].select().has_value());

  const auto back = session.select_previous_window();
  ASSERT_TRUE(back.has_value()) << back.error().diagnostic;
  EXPECT_EQ(back->id(), windows.front().id());
}

TEST(SessionNavigation, LastWindowReturnsToThePreviouslySelected) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const auto session = open(*fixture, server);
  const auto windows = four_windows(session);
  ASSERT_EQ(windows.size(), 4U);

  // Jumping somewhere not adjacent is what separates this from "next":
  // from the third window, next would land on the fourth.
  ASSERT_TRUE(windows.front().select().has_value());
  ASSERT_TRUE(windows[2].select().has_value());

  const auto last = session.select_last_window();
  ASSERT_TRUE(last.has_value()) << last.error().diagnostic;
  EXPECT_EQ(last->id(), windows.front().id());
}

TEST(SessionNavigation, NavigationRefusesWhenThereIsNowhereToGo) {
  // tmux answers "no next window" rather than reselecting the only one, and
  // saying so is more useful than a call that silently did nothing.
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;
  const auto windows = session->windows();
  ASSERT_TRUE(windows.has_value());
  ASSERT_EQ(windows->size(), 1U);

  for (const auto& moved :
       {session->select_next_window(), session->select_previous_window(),
        session->select_last_window()}) {
    ASSERT_FALSE(moved.has_value());
    EXPECT_EQ(moved.error().kind, libtmux::FailureKind::refused);
    EXPECT_FALSE(moved.error().diagnostic.empty());
  }
}

} // namespace
