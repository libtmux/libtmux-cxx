// Asking tmux to expand a format, and telling a client something.
//
// The two halves of `display-message`. They are separate methods here because
// they answer differently: one returns what tmux said, the other returns
// whether tmux was told.

#include <chrono>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "libtmux/entities.hpp"
#include "libtmux/format.hpp"
#include "libtmux/server.hpp"
#include "support/scoped_tmux_server.hpp"

namespace {

using namespace std::chrono_literals;
using libtmux::Server;

Server connect(const libtmux::test::ScopedTmuxServer& fixture) {
  auto server = Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value());
  return server.value();
}

TEST(Expand, EachPaneAnswersAboutItselfRatherThanTheActiveOne) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;
  const auto window = session->active_window();
  ASSERT_TRUE(window.has_value()) << window.error().diagnostic;
  const auto second = window->split();
  ASSERT_TRUE(second.has_value()) << second.error().diagnostic;

  // Two panes, because a format expanded without a target answers about the
  // active one: with a single pane every wrong answer is also the right one.
  const auto panes = window->panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  ASSERT_EQ(panes->size(), 2U);

  for (const libtmux::Pane& pane : *panes) {
    const auto answered = pane.expand("#{pane_id}");
    ASSERT_TRUE(answered.has_value()) << answered.error().diagnostic;
    EXPECT_EQ(*answered, pane.id());
  }
}

TEST(Expand, APaneThatHasGoneIsReportedRatherThanBlank) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;
  const auto window = session->active_window();
  ASSERT_TRUE(window.has_value()) << window.error().diagnostic;
  const auto doomed = window->split();
  ASSERT_TRUE(doomed.has_value()) << doomed.error().diagnostic;
  ASSERT_TRUE(doomed->expand("#{pane_id}").has_value());

  ASSERT_TRUE(doomed->kill().has_value());

  // tmux answers a target it cannot resolve with an empty expansion and a
  // zero exit status, which is why this is a failure rather than an empty
  // string that reads like a value.
  const auto gone = doomed->expand("#{pane_current_command}");
  ASSERT_FALSE(gone.has_value());
  EXPECT_EQ(gone.error().kind, libtmux::FailureKind::missing);
  EXPECT_NE(gone.error().diagnostic.find(doomed->id()), std::string::npos);
}

TEST(Expand, TheAnswerKeepsTheNewlinesTheFormatAsksFor) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;

  // tmux ends every expansion with a newline of its own. Removing exactly one
  // is what makes these three cases distinguishable rather than all alike.
  const auto plain = session->expand("one line");
  ASSERT_TRUE(plain.has_value()) << plain.error().diagnostic;
  EXPECT_EQ(*plain, "one line");

  const auto within = session->expand("first\nsecond");
  ASSERT_TRUE(within.has_value()) << within.error().diagnostic;
  EXPECT_EQ(*within, "first\nsecond");

  const auto trailing = session->expand("ends with one\n");
  ASSERT_TRUE(trailing.has_value()) << trailing.error().diagnostic;
  EXPECT_EQ(*trailing, "ends with one\n");

  const auto nothing = session->expand("");
  ASSERT_TRUE(nothing.has_value()) << nothing.error().diagnostic;
  EXPECT_EQ(*nothing, "");
}

TEST(Expand, AFormatMayContainTheSeparatorTheGuardUses) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;

  // The identity the guard reads is separated from the answer by the same
  // character rows are split on, so a caller using it must not lose it.
  const auto answered = session->expand("before␞after␞last");
  ASSERT_TRUE(answered.has_value()) << answered.error().diagnostic;
  EXPECT_EQ(*answered, "before␞after␞last");
}

TEST(Expand, AWindowAnswersAboutItselfAndTheServerAboutTheServer) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;
  libtmux::NewWindowOptions options;
  options.name = "second";
  const auto made = session->new_window(options);
  ASSERT_TRUE(made.has_value()) << made.error().diagnostic;

  const auto named = made->expand("#{window_name}");
  ASSERT_TRUE(named.has_value()) << named.error().diagnostic;
  EXPECT_EQ(*named, "second");

  // The server form takes no target, so what it can answer is what tmux
  // knows without one. The socket is the fixture's, which is how this says
  // more than "some string came back".
  const auto socket = server.expand("#{socket_path}");
  ASSERT_TRUE(socket.has_value()) << socket.error().diagnostic;
  EXPECT_EQ(*socket, fixture->socket_path().string());
}

TEST(Expand, LiteralTextIsTheCallersToEscape) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  // A `#` in text tmux is asked to expand starts a substitution, so text
  // built from data has to say it is text. Both halves are asserted: the
  // escape works, and leaving it out really does change the answer.
  const auto escaped = server.expand(libtmux::escape_literal("branch #{main}"));
  ASSERT_TRUE(escaped.has_value()) << escaped.error().diagnostic;
  EXPECT_EQ(*escaped, "branch #{main}");

  const auto raw = server.expand("branch #{main}");
  ASSERT_TRUE(raw.has_value()) << raw.error().diagnostic;
  EXPECT_NE(*raw, "branch #{main}");
}

TEST(ShowMessage, AMessageReachesAnAttachedControlClient) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  // A control client is the only observer available to a test: a status line
  // needs a terminal, and tmux hands the same message to a control client as
  // `%message`.
  auto watching = server.over_control(fixture->session_name());
  ASSERT_TRUE(watching.has_value()) << watching.error().diagnostic;
  (void)watching->take_notifications();

  ASSERT_TRUE(server.show_message("marker-from-the-test").has_value());

  const auto deadline = std::chrono::steady_clock::now() + 5s;
  bool seen = false;
  while (!seen && std::chrono::steady_clock::now() < deadline) {
    for (const libtmux::Notification& notification : watching->take_notifications()) {
      const std::string body{reinterpret_cast<const char*>(notification.body.data()),
                             notification.body.size()};
      seen = seen || body.find("%message marker-from-the-test") != std::string::npos;
    }
    if (!seen) {
      std::this_thread::sleep_for(5ms);
    }
  }
  EXPECT_TRUE(seen) << "no %message reached the control client";
}

TEST(ShowMessage, TheTargetIsTheContextTheTextExpandsIn) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;
  libtmux::NewWindowOptions options;
  options.name = "quiet";
  const auto quiet = session->new_window(options);
  ASSERT_TRUE(quiet.has_value()) << quiet.error().diagnostic;
  const auto active = session->active_window();
  ASSERT_TRUE(active.has_value()) << active.error().diagnostic;
  ASSERT_NE(active->id(), quiet->id());

  auto watching = server.over_control(fixture->session_name());
  ASSERT_TRUE(watching.has_value()) << watching.error().diagnostic;
  (void)watching->take_notifications();

  // Sent from the window that is not active: tmux expands the text against
  // the target, so this says which window it came from and would say the
  // other one if the target were being ignored.
  ASSERT_TRUE(quiet->show_message("sent from #{window_name}").has_value());

  const auto deadline = std::chrono::steady_clock::now() + 5s;
  std::string seen;
  while (seen.empty() && std::chrono::steady_clock::now() < deadline) {
    for (const libtmux::Notification& notification : watching->take_notifications()) {
      const std::string body{reinterpret_cast<const char*>(notification.body.data()),
                             notification.body.size()};
      if (body.find("%message sent from ") != std::string::npos) {
        seen = body;
      }
    }
    if (seen.empty()) {
      std::this_thread::sleep_for(5ms);
    }
  }
  EXPECT_NE(seen.find("sent from quiet"), std::string::npos) << seen;
}

TEST(ShowMessage, APaneNamesItselfRatherThanTheActiveOne) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;
  const auto window = session->active_window();
  ASSERT_TRUE(window.has_value()) << window.error().diagnostic;
  const auto second = window->split();
  ASSERT_TRUE(second.has_value()) << second.error().diagnostic;
  const auto active = session->active_pane();
  ASSERT_TRUE(active.has_value()) << active.error().diagnostic;
  const auto quiet = active->id() == second->id() ? window->panes()->front() : *second;
  ASSERT_NE(quiet.id(), active->id());

  auto watching = server.over_control(fixture->session_name());
  ASSERT_TRUE(watching.has_value()) << watching.error().diagnostic;
  (void)watching->take_notifications();

  ASSERT_TRUE(quiet.show_message("sent by #{pane_id}").has_value());

  const auto deadline = std::chrono::steady_clock::now() + 5s;
  std::string seen;
  while (seen.empty() && std::chrono::steady_clock::now() < deadline) {
    for (const libtmux::Notification& notification : watching->take_notifications()) {
      const std::string body{reinterpret_cast<const char*>(notification.body.data()),
                             notification.body.size()};
      if (body.find("%message sent by ") != std::string::npos) {
        seen = body;
      }
    }
    if (seen.empty()) {
      std::this_thread::sleep_for(5ms);
    }
  }
  EXPECT_NE(seen.find("sent by " + std::string{quiet.id()}), std::string::npos) << seen;
}

} // namespace
