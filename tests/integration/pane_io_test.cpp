// Reading a pane, and dropping what it remembers.
//
// Behavior evidence for the pane-io shard of the parity ledger.

#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "libtmux/capture.hpp"
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

TEST(PaneIo, JoinedCaptureHonorsTrailingSpaceOptions) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto pane = server.pane(fixture->session_name());
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;
  ASSERT_TRUE(
      server
          .run({"resize-window", "-t", fixture->session_name(), "-x", "20", "-y", "5"})
          .has_value());

  constexpr std::string_view channel{"pane-capture-options-ready"};
  ASSERT_TRUE(pane->send_text("printf '\\033[2J\\033[Hleft  middle   \\ntail   '; "
                              "tmux wait-for -S " +
                              std::string{channel} + "; exec tail -f /dev/null")
                  .has_value());
  ASSERT_TRUE(pane->send_key("Enter").has_value());
  const auto ready = server.wait_for(channel, std::chrono::seconds{5});
  ASSERT_TRUE(ready.has_value()) << ready.error().diagnostic;

  const auto trimmed = pane->capture({.join_wrapped = true});
  ASSERT_TRUE(trimmed.has_value()) << trimmed.error().diagnostic;
  const auto trimmed_lines = libtmux::capture_lines(*trimmed);
  ASSERT_GE(trimmed_lines.size(), 2U);
  EXPECT_EQ(trimmed_lines[0], "left  middle");
  EXPECT_EQ(trimmed_lines[1], "tail");

  const auto preserved =
      pane->capture({.join_wrapped = true, .keep_trailing_spaces = true});
  ASSERT_TRUE(preserved.has_value()) << preserved.error().diagnostic;
  const auto preserved_lines = libtmux::capture_lines(*preserved);
  ASSERT_GE(preserved_lines.size(), 2U);
  EXPECT_TRUE(preserved_lines[0].starts_with("left  middle   "));
  EXPECT_TRUE(preserved_lines[1].starts_with("tail   "));
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

// `wait_for_text` — the four things a supervising caller needs it to get
// right. The mechanism these exercise lived in the MCP server until it moved
// here; the reason it is worth a core test is the third one.

TEST(PaneIo, WaitForTextSeesOutputThatArrivesAfterTheWaitBegins) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto pane = server.pane(fixture->session_name());
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;

  // Delayed, and spelled so that the command does not contain its own output:
  // the shell composes "delayed-marker" only when it runs. Writing the marker
  // literally would put it on screen as the echoed prompt line before the wait
  // even began, which is a different thing to test.
  const std::string command = "sh -c 'sleep 0.3; echo delayed-$(echo marker)'";
  ASSERT_TRUE(pane->send_line(command).has_value());

  libtmux::WaitOptions options;
  options.timeout = std::chrono::seconds{5};
  options.sent = [&command](std::string_view) {
    return std::vector<std::string>{command};
  };
  const auto waited = pane->wait_for_text("delayed-marker", options);
  ASSERT_TRUE(waited.has_value()) << waited.error().diagnostic;
  EXPECT_TRUE(waited->matched) << waited->text;
  EXPECT_FALSE(waited->timed_out);
  EXPECT_FALSE(waited->matched_at_entry)
      << "the marker was not on screen when the wait began";
}

TEST(PaneIo, WaitForTextCreditsTextAlreadyOnScreenAtEntry) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto pane = server.pane(fixture->session_name());
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;

  const std::string command = "echo settled-marker";
  ASSERT_TRUE(pane->send_line(command).has_value());
  libtmux::WaitOptions settle;
  settle.timeout = std::chrono::seconds{5};
  settle.sent = [&command](std::string_view) {
    return std::vector<std::string>{command};
  };
  ASSERT_TRUE(pane->wait_for_text("settled-marker", settle).has_value());

  libtmux::WaitOptions options;
  options.timeout = std::chrono::seconds{5};
  options.sent = [&command](std::string_view) {
    return std::vector<std::string>{command};
  };
  const auto waited = pane->wait_for_text("settled-marker", options);
  ASSERT_TRUE(waited.has_value()) << waited.error().diagnostic;
  EXPECT_TRUE(waited->matched) << waited->text;
  EXPECT_TRUE(waited->matched_at_entry);
  EXPECT_EQ(waited->path, libtmux::WaitPath::capture_at_entry);
}

// The one that is hard to get right, and the reason this belongs in the
// library rather than in each consumer. A command sitting on the prompt has
// been typed and not run: searching the screen for it succeeds immediately,
// and reporting that as output is wrong.
TEST(PaneIo, WaitForTextDoesNotCreditACallersOwnEchoAsOutput) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto pane = server.pane(fixture->session_name());
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;

  // Typed, never submitted: the shell echoes it onto the prompt and nothing
  // runs it, so the pane has produced nothing.
  const std::string typed = "never-submitted-marker";
  ASSERT_TRUE(pane->send_text(typed).has_value());

  libtmux::WaitOptions options;
  options.timeout = std::chrono::milliseconds{1500};
  options.sent = [&typed](std::string_view) { return std::vector<std::string>{typed}; };
  const auto waited = pane->wait_for_text(typed, options);
  ASSERT_TRUE(waited.has_value()) << waited.error().diagnostic;
  EXPECT_FALSE(waited->matched)
      << "the echo of a command is not its output: " << waited->text;
  EXPECT_TRUE(waited->timed_out);
  // Not a silent timeout: it is on screen, and the caller is told so.
  EXPECT_TRUE(waited->present_unconfirmed);
}

TEST(PaneIo, WaitForTextTimesOutRatherThanHangingOnAQuietPane) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto pane = server.pane(fixture->session_name());
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;

  libtmux::WaitOptions options;
  options.timeout = std::chrono::milliseconds{750};
  const auto waited = pane->wait_for_text("text-that-never-appears", options);
  ASSERT_TRUE(waited.has_value()) << waited.error().diagnostic;
  EXPECT_FALSE(waited->matched);
  EXPECT_TRUE(waited->timed_out);
  EXPECT_FALSE(waited->present_unconfirmed)
      << "nothing resembling the wanted text was ever on screen";
  EXPECT_GE(waited->elapsed, std::chrono::milliseconds{750});
}

TEST(PaneIo, WaitForTextAnswersCancellationRatherThanTheDeadline) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto pane = server.pane(fixture->session_name());
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;

  // A caller that has already given up: the wait must answer that, and must
  // not spend the budget first.
  libtmux::WaitOptions options;
  options.timeout = std::chrono::seconds{30};
  options.cancelled = [] { return true; };
  const auto started = std::chrono::steady_clock::now();
  const auto waited = pane->wait_for_text("text-that-never-appears", options);
  ASSERT_FALSE(waited.has_value()) << "a cancelled wait is a failure, not a timeout";
  EXPECT_EQ(waited.error().kind, libtmux::FailureKind::cancelled);
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds{5});
}
