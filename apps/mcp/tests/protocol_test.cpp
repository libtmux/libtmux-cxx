// The MCP server as a program: JSON-RPC in, JSON-RPC out, tmux underneath.
//
// The tool surface has a test that calls it directly, and the packaging lane
// speaks protocol to the installed binary with no tmux behind it. Between
// those sat `tools/call` — argument decoding, the content framing, and the
// distinction between a model's mistake and tmux's refusal — which nothing
// executed. That is the only path an MCP client ever takes.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <libtmux/testing/scoped_server.hpp>

#include "run_server.hpp"

namespace {

using json = nlohmann::json;
using libtmux::test::ScopedTmuxServer;
using libtmux::test::ScopedTmuxServerOptions;
using libtmux::test::SocketNamespace;

// Speak a whole session to the server and return one reply per request that
// asked for an answer. The server reads stdin to end, so a conversation is a
// batch: this is the shape an MCP host's pipe has, not a limitation.
std::vector<json> converse(const std::filesystem::path& socket,
                           const std::vector<json>& requests) {
  std::string input;
  for (const json& request : requests) {
    input += request.dump();
    input += '\n';
  }

  auto finished = libtmux::mcp::test::run_server(
      LIBTMUX_MCP_SERVER_PATH, {socket.string()}, libtmux::test::current_environment(),
      input, std::chrono::seconds{60});
  EXPECT_TRUE(finished.has_value()) << finished.error();
  if (!finished.has_value()) {
    return {};
  }

  std::vector<json> replies;
  std::size_t start = 0;
  const std::string& out = *finished;
  while (start < out.size()) {
    const auto end = out.find('\n', start);
    const auto line = out.substr(start, end == std::string::npos ? end : end - start);
    if (!line.empty()) {
      replies.push_back(json::parse(line, nullptr, false));
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return replies;
}

json call(const std::string& name, const json& arguments, int id = 2) {
  return json{{"jsonrpc", "2.0"},
              {"id", id},
              {"method", "tools/call"},
              {"params", {{"name", name}, {"arguments", arguments}}}};
}

class McpProtocol : public testing::Test {
protected:
  void SetUp() override {
    auto started = ScopedTmuxServer::start(ScopedTmuxServerOptions{
        .session_name = "mcp", .socket_namespace = SocketNamespace::consumer("mcp")});
    ASSERT_TRUE(started.has_value()) << started.error();
    fixture_ = std::make_unique<ScopedTmuxServer>(*std::move(started));
  }

  [[nodiscard]] const std::filesystem::path& socket() const {
    return fixture_->socket_path();
  }

  std::unique_ptr<ScopedTmuxServer> fixture_;
};

TEST_F(McpProtocol, ListsToolsWithADescriptionForEveryParameter) {
  const auto replies = converse(
      socket(), {json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}});
  ASSERT_EQ(replies.size(), 1U);
  const auto& tools = replies[0]["result"]["tools"];
  ASSERT_TRUE(tools.is_array());
  EXPECT_FALSE(tools.empty());

  for (const auto& tool : tools) {
    EXPECT_FALSE(tool["description"].get<std::string>().empty()) << tool["name"];
    const auto& schema = tool["inputSchema"];
    EXPECT_EQ(schema["type"], "object");
    // A model is given the parameter and nothing else unless this holds.
    for (const auto& [name, property] : schema["properties"].items()) {
      EXPECT_EQ(property["type"], "string") << name;
      EXPECT_FALSE(property.value("description", "").empty())
          << tool["name"] << '.' << name << " has no description";
    }
    // Everything required must be a declared property; the two used to be the
    // same list, which is why an optional parameter could not exist.
    for (const auto& required : schema["required"]) {
      EXPECT_TRUE(schema["properties"].contains(required.get<std::string>()))
          << tool["name"] << " requires an undeclared " << required;
    }
  }
}

TEST_F(McpProtocol, CallsAToolAgainstTmuxAndFramesTheAnswer) {
  const auto replies = converse(socket(), {call("list_sessions", json::object())});
  ASSERT_EQ(replies.size(), 1U);
  const auto& result = replies[0]["result"];
  ASSERT_TRUE(result.contains("content")) << replies[0].dump();
  EXPECT_FALSE(result["isError"].get<bool>());
  ASSERT_EQ(result["content"].size(), 1U);
  EXPECT_EQ(result["content"][0]["type"], "text");
  EXPECT_NE(result["content"][0]["text"].get<std::string>().find("mcp"),
            std::string::npos);
}

TEST_F(McpProtocol, SeparatesACallerMistakeFromATmuxRefusal) {
  const auto replies =
      converse(socket(), {
                             call("capture_pane", json::object(), 2),
                             call("no_such_tool", json::object(), 3),
                             call("capture_pane", {{"target", "%999"}}, 4),
                         });
  ASSERT_EQ(replies.size(), 3U);

  // A missing argument and an unknown tool are the model calling wrongly, so
  // they are protocol errors it must fix.
  EXPECT_TRUE(replies[0].contains("error")) << replies[0].dump();
  EXPECT_TRUE(replies[1].contains("error")) << replies[1].dump();

  // tmux refusing a well-formed request is a result the model should read.
  ASSERT_TRUE(replies[2].contains("result")) << replies[2].dump();
  EXPECT_TRUE(replies[2]["result"]["isError"].get<bool>());
  EXPECT_FALSE(replies[2]["result"]["content"][0]["text"].get<std::string>().empty());
}

TEST_F(McpProtocol, CarriesArgumentsThroughToTmux) {
  const auto replies = converse(
      socket(), {call("new_window", {{"session", "mcp"}, {"name", "from-protocol"}}, 2),
                 call("list_panes", json::object(), 3)});
  ASSERT_EQ(replies.size(), 2U);

  ASSERT_TRUE(replies[0].contains("result")) << replies[0].dump();
  const auto window = replies[0]["result"]["content"][0]["text"].get<std::string>();
  EXPECT_EQ(window.front(), '@');

  const auto panes = replies[1]["result"]["content"][0]["text"].get<std::string>();
  EXPECT_NE(panes.find(window), std::string::npos) << panes;
}

// A number is not a string, and the surface takes named strings. Coercing
// rather than refusing is what keeps a model from having to learn which
// arguments are quoted.
TEST_F(McpProtocol, AcceptsANumberWhereAStringIsExpected) {
  const auto replies = converse(socket(), {call("capture_pane", {{"target", 999}})});
  ASSERT_EQ(replies.size(), 1U);
  // 999 is not a pane, so this is tmux refusing — not the argument decoder.
  ASSERT_TRUE(replies[0].contains("result")) << replies[0].dump();
  EXPECT_TRUE(replies[0]["result"]["isError"].get<bool>());
}

// The three that let a model do more than look: press a key, wait for
// something to finish, and find where it happened.

TEST_F(McpProtocol, PressesKeysByNameAndRefusesOneItDoesNotKnow) {
  const auto replies = converse(
      socket(), {call("list_panes", json::object(), 1),
                 call("send_keys", {{"target", "mcp"}, {"keys", "Enter"}}, 2),
                 call("send_keys", {{"target", "mcp"}, {"keys", "NotAKey"}}, 3)});
  ASSERT_EQ(replies.size(), 3U);

  ASSERT_TRUE(replies[1].contains("result")) << replies[1].dump();
  EXPECT_FALSE(replies[1]["result"]["isError"].get<bool>());
  EXPECT_EQ(replies[1]["result"]["content"][0]["text"].get<std::string>().front(), '%');

  // A mistyped key name is the model's mistake, and must not reach the pane as
  // stray characters.
  EXPECT_TRUE(replies[2].contains("error")) << replies[2].dump();
}

TEST_F(McpProtocol, WaitsForTextAndReportsWhenItNeverArrives) {
  const auto replies = converse(
      socket(),
      {call("send_keys", {{"target", "mcp"}, {"keys", "Enter"}}, 1),
       call("send_text", {{"target", "mcp"}, {"text", "echo waited-for-this\n"}}, 2),
       call("wait_for_text", {{"target", "mcp"}, {"text", "waited-for-this"}}, 3),
       call("wait_for_text",
            {{"target", "mcp"}, {"text", "never-appears"}, {"timeout_ms", "300"}}, 4)});
  ASSERT_EQ(replies.size(), 4U);

  ASSERT_TRUE(replies[2].contains("result")) << replies[2].dump();
  EXPECT_NE(replies[2]["result"]["content"][0]["text"].get<std::string>().find(
                "waited-for-this"),
            std::string::npos);

  // Not finding it is an answer, not a failure — and it says so rather than
  // returning an empty capture the model would have to interpret.
  ASSERT_TRUE(replies[3].contains("result")) << replies[3].dump();
  EXPECT_NE(
      replies[3]["result"]["content"][0]["text"].get<std::string>().find("timed out"),
      std::string::npos);
}

TEST_F(McpProtocol, FindsWhichPaneIsShowingSomething) {
  const auto replies = converse(
      socket(),
      {call("send_text", {{"target", "mcp"}, {"text", "echo find-me-here\n"}}, 1),
       call("wait_for_text", {{"target", "mcp"}, {"text", "find-me-here"}}, 2),
       call("search_panes", {{"text", "find-me-here"}}, 3),
       call("search_panes", {{"text", "nothing-shows-this"}}, 4)});
  ASSERT_EQ(replies.size(), 4U);

  ASSERT_TRUE(replies[2].contains("result")) << replies[2].dump();
  const auto found = replies[2]["result"]["content"][0]["text"].get<std::string>();
  EXPECT_EQ(found.front(), '%') << found;
  EXPECT_NE(found.find("find-me-here"), std::string::npos) << found;

  EXPECT_TRUE(replies[3]["result"]["content"][0]["text"].get<std::string>().empty());
}

// An optional argument is optional, which is the whole reason the schema grew
// a way to say so.
TEST_F(McpProtocol, TakesTheOptionalTimeoutOrDoesWithoutIt) {
  const auto replies = converse(
      socket(), {json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}});
  ASSERT_EQ(replies.size(), 1U);
  for (const auto& tool : replies[0]["result"]["tools"]) {
    if (tool["name"] != "wait_for_text") {
      continue;
    }
    const auto& schema = tool["inputSchema"];
    EXPECT_TRUE(schema["properties"].contains("timeout_ms"));
    const auto& required = schema["required"];
    EXPECT_EQ(std::find(required.begin(), required.end(), "timeout_ms"), required.end())
        << "timeout_ms is declared required";
    return;
  }
  ADD_FAILURE() << "wait_for_text is not in tools/list";
}

} // namespace
