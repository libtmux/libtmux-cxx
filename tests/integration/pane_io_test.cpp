// Reading a pane, and dropping what it remembers.
//
// Behavior evidence for the pane-io shard of the parity ledger.

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

TEST(PaneIo, ClearHistoryDropsTheScrollbackAndKeepsTheScreen) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;
  const auto pane = session->active_pane();
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;

  // Enough lines to push some off the visible screen and into scrollback.
  for (int line = 0; line < 60; ++line) {
    ASSERT_TRUE(pane->send_text("echo scrollback-" + std::to_string(line)).has_value());
    ASSERT_TRUE(pane->send_key("Enter").has_value());
  }
  libtmux::CaptureOptions history;
  history.whole_history = true;
  const auto before = pane->capture(history);
  ASSERT_TRUE(before.has_value()) << before.error().diagnostic;
  ASSERT_NE(before->find("scrollback-0"), std::string::npos)
      << "the early lines should have reached the scrollback";

  ASSERT_TRUE(pane->clear_history().has_value());

  // The scrollback is gone; what is still on screen is not, because
  // clear-history drops history rather than clearing the display.
  const auto after = pane->capture(history);
  ASSERT_TRUE(after.has_value()) << after.error().diagnostic;
  EXPECT_EQ(after->find("scrollback-0"), std::string::npos);
  EXPECT_NE(after->find("scrollback-59"), std::string::npos);
}

TEST(PaneIo, CopyModeIsEnteredAndLeftWithoutAnAttachedClient) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;
  const auto pane = session->active_pane();
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;
  EXPECT_FALSE(pane->in_mode());

  ASSERT_TRUE(pane->enter_copy_mode().has_value());
  const auto inside = pane->refresh();
  ASSERT_TRUE(inside.has_value()) << inside.error().diagnostic;
  EXPECT_TRUE(inside->in_mode());

  // Entering again is harmless, so a caller does not have to ask first.
  EXPECT_TRUE(pane->enter_copy_mode().has_value());

  ASSERT_TRUE(pane->leave_mode().has_value());
  const auto outside = pane->refresh();
  ASSERT_TRUE(outside.has_value()) << outside.error().diagnostic;
  EXPECT_FALSE(outside->in_mode());

  // Leaving again is refused rather than quietly succeeding: tmux answers
  // "not in a mode", and the caller sees that instead of a success that
  // meant nothing.
  const auto again = pane->leave_mode();
  ASSERT_FALSE(again.has_value());
  EXPECT_EQ(again.error().kind, libtmux::FailureKind::refused);
  EXPECT_FALSE(again.error().diagnostic.empty());
}

} // namespace
