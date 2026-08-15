// What this tmux says it can do.

#include <algorithm>
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

TEST(Commands, TheServerNamesEveryCommandItUnderstands) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto commands = server.commands();
  ASSERT_TRUE(commands.has_value()) << commands.error().diagnostic;
  // Every supported tmux has far more than this; the bound is here to catch
  // a listing that parsed into nothing rather than to count features.
  ASSERT_GT(commands->size(), 40U);

  const auto named = [&](std::string_view want) {
    return std::ranges::any_of(
        *commands, [want](const libtmux::Command& one) { return one.name() == want; });
  };
  EXPECT_TRUE(named("new-session"));
  EXPECT_TRUE(named("list-commands"));
  EXPECT_FALSE(named("no-such-command"));

  // The alias and usage come from the same row, so a command found by name
  // carries the rest of what tmux says about it.
  const auto listing = std::ranges::find_if(*commands, [](const libtmux::Command& one) {
    return one.name() == "list-commands";
  });
  ASSERT_NE(listing, commands->end());
  EXPECT_EQ(listing->alias(), "lscm");

  const auto attach = std::ranges::find_if(*commands, [](const libtmux::Command& one) {
    return one.name() == "attach-session";
  });
  ASSERT_NE(attach, commands->end());
  EXPECT_NE(attach->usage().find("-t target-session"), std::string_view::npos);
}

} // namespace
