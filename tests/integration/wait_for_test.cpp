// Waiting on a channel, and the two ways that lies.

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "libtmux/server.hpp"
#include "libtmux/testing/scoped_server.hpp"

namespace {

using libtmux::Server;
using namespace std::chrono_literals;

Server connect(const libtmux::test::ScopedTmuxServer& fixture) {
  auto server = Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value());
  return server.value();
}

TEST(WaitFor, ASignalSentBeforeTheWaitStillReleasesIt) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  // Latched, not edge-triggered: signalling first is safe, which is what
  // lets two processes race without losing the exchange.
  ASSERT_TRUE(server.signal("early").has_value());
  const auto released = server.wait_for("early", 5s);
  EXPECT_TRUE(released.has_value())
      << (released.has_value() ? "" : released.error().diagnostic);
}

TEST(WaitFor, LeadingDashesBelongToTheChannelName) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  ASSERT_TRUE(server.run({"wait-for", "-S", "--", "-L"}).has_value());
  const auto waited = server.wait_for("-L", 250ms);
  EXPECT_TRUE(waited.has_value())
      << (waited.has_value() ? "" : waited.error().diagnostic);

  const auto signalled = server.signal("-S");
  ASSERT_TRUE(signalled.has_value()) << signalled.error().diagnostic;
  EXPECT_TRUE(server.run({"wait-for", "--", "-S"}, 250ms).has_value());
}

TEST(WaitFor, ASignalFromAnotherThreadReleasesTheWaiter) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  std::thread signaller{[&] {
    std::this_thread::sleep_for(300ms);
    static_cast<void>(server.signal("late"));
  }};
  const auto released = server.wait_for("late", 10s);
  signaller.join();
  EXPECT_TRUE(released.has_value())
      << (released.has_value() ? "" : released.error().diagnostic);
}

TEST(WaitFor, NobodySignallingIsATimeoutRatherThanAHang) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto started = std::chrono::steady_clock::now();
  const auto waited = server.wait_for("silent", 500ms);
  const auto elapsed = std::chrono::steady_clock::now() - started;

  ASSERT_FALSE(waited.has_value());
  EXPECT_EQ(waited.error().kind, libtmux::FailureKind::timeout);
  EXPECT_LT(elapsed, 10s);
}

TEST(WaitFor, AServerDyingUnderTheWaiterIsNotASignal) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  // tmux exits zero here. Reported as success, a caller would carry on as
  // though the other side had spoken, which is the whole reason this call
  // exists rather than running the command.
  std::thread killer{[&] {
    std::this_thread::sleep_for(400ms);
    static_cast<void>(server.kill());
  }};
  const auto waited = server.wait_for("doomed", 10s);
  killer.join();

  ASSERT_FALSE(waited.has_value()) << "a dead server must not read as a signal";
  EXPECT_FALSE(waited.error().diagnostic.empty());
}

TEST(WaitFor, AChannelNeedsAName) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto waited = server.wait_for("", 1s);
  ASSERT_FALSE(waited.has_value());
  EXPECT_EQ(waited.error().kind, libtmux::FailureKind::validation);
  EXPECT_EQ(waited.error().delivery, libtmux::DeliveryStatus::not_started);
  EXPECT_FALSE(server.signal("").has_value());
}

} // namespace
