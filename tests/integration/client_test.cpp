// Clients, which are the one part of tmux this library cannot become.
//
// A tmux client needs a terminal, and every command here talks to tmux over
// pipes. So the library observes and commands clients rather than being one,
// and hands back the command line for a caller that does own a terminal.
//
// A control-mode connection is a client without a terminal, which is what
// makes these testable at all.

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "libtmux/entities.hpp"
#include "libtmux/server.hpp"
#include "support/scoped_tmux_server.hpp"
#include "support/tmux_capabilities.hpp"

namespace {

using libtmux::Client;
using libtmux::Server;
using libtmux::Session;

Server connect(const libtmux::test::ScopedTmuxServer& fixture) {
  auto server = Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value());
  return server.value();
}

// A control-mode client attaches without a terminal, but tmux registers it a
// moment after the connection returns.
std::vector<Client> clients_once_attached(const Server& server) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    auto clients = server.clients();
    EXPECT_TRUE(clients.has_value());
    if (!clients->empty()) {
      return *std::move(clients);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  return {};
}

TEST(Client, AControlConnectionIsAClientTheServerReports) {
  LIBTMUX_REQUIRES_TMUX(3, 3, "display-message -c against a control client");

  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto listed_before = server.clients();
  ASSERT_TRUE(listed_before.has_value()) << listed_before.error().diagnostic;
  EXPECT_TRUE(listed_before->empty());

  const auto control = server.control(fixture->session_name());
  ASSERT_TRUE(control.has_value()) << control.error().message;

  const std::vector<Client> clients = clients_once_attached(server);
  ASSERT_EQ(clients.size(), 1U);
  const Client& client = clients.front();
  EXPECT_TRUE(client.control_mode());
  EXPECT_FALSE(client.name().empty());
  EXPECT_EQ(client.session_name(), fixture->session_name());

  const auto attached = client.session();
  ASSERT_TRUE(attached.has_value()) << attached.error().diagnostic;
  EXPECT_EQ(attached->name(), fixture->session_name());
}

TEST(Client, AClientIsPointedAtAnotherSessionAndThenSentAway) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto elsewhere = server.new_session("elsewhere");
  ASSERT_TRUE(elsewhere.has_value()) << elsewhere.error().diagnostic;

  const auto control = server.control(fixture->session_name());
  ASSERT_TRUE(control.has_value()) << control.error().message;
  const std::vector<Client> clients = clients_once_attached(server);
  ASSERT_EQ(clients.size(), 1U);

  ASSERT_TRUE(clients.front().switch_to(*elsewhere).has_value());
  const auto moved = server.clients();
  ASSERT_TRUE(moved.has_value()) << moved.error().diagnostic;
  ASSERT_EQ(moved->size(), 1U);
  EXPECT_EQ(moved->front().session_name(), "elsewhere");

  ASSERT_TRUE(moved->front().detach().has_value());
  for (int attempt = 0; attempt < 200; ++attempt) {
    const auto remaining = server.clients();
    ASSERT_TRUE(remaining.has_value()) << remaining.error().diagnostic;
    if (remaining->empty()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  const auto remaining = server.clients();
  ASSERT_TRUE(remaining.has_value()) << remaining.error().diagnostic;
  EXPECT_TRUE(remaining->empty());
  // The session outlives the client that was looking at it.
  EXPECT_TRUE(server.is_alive());
}

TEST(Client, ASessionSendsEveryClientAway) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto control = server.control(fixture->session_name());
  ASSERT_TRUE(control.has_value()) << control.error().message;
  ASSERT_EQ(clients_once_attached(server).size(), 1U);

  auto sessions = server.sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  ASSERT_TRUE(sessions->front().detach_clients().has_value());

  for (int attempt = 0; attempt < 200; ++attempt) {
    const auto remaining = server.clients();
    ASSERT_TRUE(remaining.has_value()) << remaining.error().diagnostic;
    if (remaining->empty()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  const auto remaining = server.clients();
  ASSERT_TRUE(remaining.has_value()) << remaining.error().diagnostic;
  EXPECT_TRUE(remaining->empty());
}

TEST(Client, AttachingIsACommandLineRatherThanACall) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  auto sessions = server.sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  const std::vector<std::string> command = sessions->front().attach_command();

  // Everything needed to reach this exact server, in exec order.
  ASSERT_GE(command.size(), 5U);
  EXPECT_EQ(command.front(), "tmux");
  EXPECT_EQ(command[1], "-S");
  EXPECT_EQ(command[2], fixture->socket_path().string());
  EXPECT_EQ(command[command.size() - 3], "attach-session");
  EXPECT_EQ(command[command.size() - 2], "-t");
  EXPECT_EQ(command.back(), sessions->front().id());

  // Running it through the library is exactly what does not work, and it says
  // so rather than hanging.
  const auto refused = server.run({"attach-session", "-t", command.back()});
  ASSERT_FALSE(refused.has_value());
  EXPECT_NE(refused.error().diagnostic.find("terminal"), std::string::npos);
}

} // namespace
