#include "libtmux/chain.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "libtmux/cardinality.hpp"
#include "libtmux/keys.hpp"
#include "libtmux/server.hpp"
#include "support/scoped_tmux_server.hpp"

namespace {

using libtmux::Chain;
using libtmux::first;
using libtmux::matching;
using libtmux::Server;
namespace window = libtmux::window;

TEST(Chain, StopsAtTheFirstBadStepAndNamesIt) {
  Chain chain;
  chain.new_session("work").new_window("a:b", "editor").new_window("work", "logs");
  EXPECT_FALSE(chain.valid());
  EXPECT_NE(chain.error().find("session name"), std::string::npos);
  // Accumulation stops, so a later good step cannot mask the bad one.
  EXPECT_EQ(chain.batch().size(), 1U);
}

TEST(Chain, RejectsAKeyTmuxWouldSwallow) {
  Chain chain;
  chain.send_key("%0", "NoSuchKey");
  EXPECT_FALSE(chain.valid());
  EXPECT_NE(chain.error().find("NoSuchKey"), std::string::npos);
}

TEST(Chain, SendsTextLiterallyRatherThanAsKeys) {
  Chain chain;
  chain.send_text("%0", "Enter C-a");
  ASSERT_TRUE(chain.valid());
  const auto argv = chain.batch().argv();
  EXPECT_NE(std::ranges::find(argv, "-l"), argv.end());
  EXPECT_NE(std::ranges::find(argv, "Enter C-a"), argv.end());
}

TEST(Chain, AnInvalidChainNeverReachesTmux) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto server = Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());

  Chain chain;
  chain.new_window("a:b", "never");
  const auto outcome = server->run_chain(chain);
  ASSERT_FALSE(outcome.has_value());
  EXPECT_FALSE(outcome.error().dispatched);
}

TEST(Chain, BuildsRealWindowsInOneGroup) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto server = Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());
  const std::string session{fixture->session_name()};

  Chain chain;
  chain.new_window(session, "chained").split_window(session, "chained");
  const auto outcome = server->run_chain(chain);
  ASSERT_TRUE(outcome.has_value()) << outcome.error().diagnostic;

  const auto windows = server->windows();
  ASSERT_TRUE(windows.has_value()) << windows.error().diagnostic;
  auto chained = *windows | matching(window::name == "chained");
  EXPECT_TRUE(first(chained).has_value());
}

TEST(ServerDiagnostics, CarriesTheReasonTmuxRefused) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto server = Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());

  // tmux writes why it refused to stderr, so a diagnostic built from stdout
  // alone is empty for every ordinary failure.
  const auto refused = server->run({"kill-session", "-t", "no-such-session"});
  ASSERT_FALSE(refused.has_value());
  EXPECT_TRUE(refused.error().dispatched);
  EXPECT_NE(refused.error().diagnostic.find("no-such-session"), std::string::npos)
      << "diagnostic was: " << refused.error().diagnostic;
}

TEST(KeyNames, RejectsAFunctionKeyTooLongToAccumulate) {
  EXPECT_FALSE(libtmux::is_key_name("F99999999999"));
  EXPECT_FALSE(libtmux::is_key_name("F013"));
  EXPECT_TRUE(libtmux::is_key_name("F12"));
}

} // namespace
