#include "libtmux_consumers/mcp.hpp"

#include <string>

#include <gtest/gtest.h>

#include "libtmux/testing/scoped_server.hpp"

namespace {

using libtmux::Server;
using libtmux::mcp::Arguments;
using libtmux::mcp::default_tools;

Server connect(const libtmux::test::ScopedTmuxServer& fixture) {
  auto server = Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value());
  return server.value();
}

TEST(McpTools, ListsTheSessionsOfARealServer) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto result = default_tools().call(connect(*fixture), "list_sessions", {});
  ASSERT_TRUE(result.has_value()) << result.error().message;
  EXPECT_NE(result->find(fixture->session_name()), std::string::npos);
}

TEST(McpTools, SeparatesACallerMistakeFromATmuxRefusal) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const auto tools = default_tools();

  const auto missing = tools.call(server, "capture_pane", {});
  ASSERT_FALSE(missing.has_value());
  EXPECT_TRUE(missing.error().caller_error);

  const auto unknown = tools.call(server, "no_such_tool", {});
  ASSERT_FALSE(unknown.has_value());
  EXPECT_TRUE(unknown.error().caller_error);

  const auto refused =
      tools.call(server, "capture_pane", Arguments{{"target", "%999"}});
  ASSERT_FALSE(refused.has_value());
  EXPECT_FALSE(refused.error().caller_error);
}

TEST(McpTools, CapturesAPaneThroughTheLibrary) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const auto captured =
      default_tools().call(server, "capture_pane",
                           Arguments{{"target", std::string{fixture->session_name()}}});
  ASSERT_TRUE(captured.has_value()) << captured.error().message;
}

TEST(McpTools, CreatesAWindowAndTypesIntoItsPane) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const auto tools = default_tools();
  const std::string session{fixture->session_name()};

  const auto created = tools.call(
      server, "new_window", Arguments{{"session", session}, {"name", "from-mcp"}});
  ASSERT_TRUE(created.has_value()) << created.error().message;
  EXPECT_EQ(created->front(), '@');

  const auto typed = tools.call(server, "send_text",
                                Arguments{{"target", *created}, {"text", "marker"}});
  ASSERT_TRUE(typed.has_value()) << typed.error().message;

  const auto listed = tools.call(server, "list_panes", {});
  ASSERT_TRUE(listed.has_value()) << listed.error().message;
  EXPECT_NE(listed->find(*created), std::string::npos);
}

TEST(McpTools, EveryToolDeclaresANameAndDescription) {
  // The set must be named: tools() returns a reference into it, and a
  // range-for over the temporary would outlive what it borrows.
  const auto tools = default_tools();
  for (const auto& tool : tools.tools()) {
    EXPECT_FALSE(tool.name.empty());
    EXPECT_FALSE(tool.description.empty());
  }
}

} // namespace
