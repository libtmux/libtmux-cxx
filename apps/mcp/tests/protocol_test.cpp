// Exercise the installed shape: newline-delimited JSON-RPC over stdio with a
// private real tmux server underneath.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <libtmux/testing/scoped_server.hpp>

#include "run_server.hpp"

namespace {

using json = nlohmann::json;
using libtmux::test::ScopedTmuxServer;
using libtmux::test::ScopedTmuxServerOptions;
using libtmux::test::SocketMode;
using libtmux::test::SocketNamespace;

json initialize_request(std::string_view version = "2025-06-18") {
  return json{{"jsonrpc", "2.0"},
              {"id", "initialize"},
              {"method", "initialize"},
              {"params",
               {{"protocolVersion", std::string{version}},
                {"capabilities", json::object()},
                {"clientInfo", {{"name", "libtmux-test"}, {"version", "1"}}}}}};
}

json modern_versions() { return json::array({"2026-07-28"}); }

json initialized_notification() {
  return json{{"jsonrpc", "2.0"},
              {"method", "notifications/initialized"},
              {"params", json::object()}};
}

json modern_metadata(std::optional<json> progress_token = {}) {
  json metadata{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                {"io.modelcontextprotocol/clientCapabilities", json::object()},
                {"io.modelcontextprotocol/clientInfo",
                 {{"name", "libtmux-test"}, {"version", "1"}}}};
  if (progress_token.has_value()) {
    metadata["progressToken"] = *progress_token;
  }
  return metadata;
}

json modern_request(std::string method, json id, json params = json::object(),
                    std::optional<json> progress_token = {}) {
  params["_meta"] = modern_metadata(std::move(progress_token));
  return json{{"jsonrpc", "2.0"},
              {"id", std::move(id)},
              {"method", std::move(method)},
              {"params", std::move(params)}};
}

json modern_cancel(json request_id) {
  return json{
      {"jsonrpc", "2.0"},
      {"method", "notifications/cancelled"},
      {"params", {{"requestId", std::move(request_id)}, {"reason", "test complete"}}}};
}

std::string encode_requests(const std::vector<json>& requests) {
  std::string input;
  for (const json& request : requests) {
    input += request.dump();
    input += '\n';
  }
  return input;
}

std::vector<json>
decode_messages(const libtmux::expected<std::string, std::string>& finished) {
  EXPECT_TRUE(finished.has_value()) << finished.error();
  if (!finished.has_value()) {
    return {};
  }

  std::vector<json> messages;
  std::size_t start = 0;
  while (start < finished->size()) {
    const auto end = finished->find('\n', start);
    const auto line =
        finished->substr(start, end == std::string::npos ? end : end - start);
    if (!line.empty()) {
      json parsed = json::parse(line, nullptr, false);
      EXPECT_FALSE(parsed.is_discarded()) << line;
      if (!parsed.is_discarded()) {
        messages.push_back(std::move(parsed));
      }
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1U;
  }
  return messages;
}

std::vector<json>
converse_with(std::vector<std::string> arguments, std::vector<std::string> environment,
              const std::vector<json>& requests,
              std::chrono::milliseconds linger = std::chrono::milliseconds{250}) {
  auto finished = libtmux::mcp::test::run_server(
      LIBTMUX_MCP_SERVER_PATH, std::move(arguments), std::move(environment),
      encode_requests(requests), std::chrono::seconds{60}, linger);
  return decode_messages(finished);
}

std::vector<json>
converse_raw(std::string socket_name, std::string input,
             std::chrono::milliseconds linger = std::chrono::milliseconds{250}) {
  auto finished = libtmux::mcp::test::run_server(
      LIBTMUX_MCP_SERVER_PATH, {"--socket-name", std::move(socket_name)},
      libtmux::test::current_environment(), input, std::chrono::seconds{60}, linger);
  return decode_messages(finished);
}

std::vector<json> converse_batch(json batch) {
  std::string input =
      encode_requests({initialize_request("2025-03-26"), initialized_notification()});
  input += batch.dump();
  input += '\n';
  return converse_raw("libtmux-cxx-mcp-batch-no-dispatch", std::move(input));
}

std::vector<json>
converse_steps(const std::filesystem::path& socket,
               const std::vector<libtmux::mcp::test::InputStep>& steps) {
  auto finished = libtmux::mcp::test::run_server_steps(
      LIBTMUX_MCP_SERVER_PATH, {"--socket-path", socket.string()},
      libtmux::test::current_environment(), steps, std::chrono::seconds{60});
  return decode_messages(finished);
}

std::vector<json>
converse(const std::filesystem::path& socket, const std::vector<json>& requests,
         std::chrono::milliseconds linger = std::chrono::milliseconds{250}) {
  return converse_with({"--socket-path", socket.string()},
                       libtmux::test::current_environment(), requests, linger);
}

std::vector<json>
converse_ready(const std::filesystem::path& socket, std::vector<json> requests,
               std::chrono::milliseconds linger = std::chrono::milliseconds{250}) {
  requests.insert(requests.begin(), initialized_notification());
  requests.insert(requests.begin(), initialize_request());
  return converse(socket, requests, linger);
}

const json* response(const std::vector<json>& messages, const json& id) {
  for (const json& message : messages) {
    if (message.is_array()) {
      const auto found = std::ranges::find_if(message, [&id](const json& item) {
        const auto identifier = item.find("id");
        return identifier != item.end() && *identifier == id;
      });
      if (found != message.end()) {
        return &*found;
      }
      continue;
    }
    const auto identifier = message.find("id");
    if (identifier != message.end() && *identifier == id) {
      return &message;
    }
  }
  return nullptr;
}

std::optional<std::size_t> response_position(const std::vector<json>& messages,
                                             const json& id) {
  for (std::size_t index = 0; index < messages.size(); ++index) {
    const auto identifier = messages[index].find("id");
    if (identifier != messages[index].end() && *identifier == id) {
      return index;
    }
  }
  return std::nullopt;
}

json call(const std::string& name, const json& arguments, int id,
          std::optional<json> progress_token = {}) {
  json params{{"name", name}, {"arguments", arguments}};
  if (progress_token.has_value()) {
    params["_meta"] = {{"progressToken", *progress_token}};
  }
  return json{{"jsonrpc", "2.0"},
              {"id", id},
              {"method", "tools/call"},
              {"params", std::move(params)}};
}

json modern_call(const std::string& name, const json& arguments, int id,
                 std::optional<json> progress_token = {}) {
  return modern_request("tools/call", id, {{"name", name}, {"arguments", arguments}},
                        std::move(progress_token));
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

TEST_F(McpProtocol, EnforcesTheInitializationLifecycle) {
  const auto messages = converse(
      socket(), {json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}},
                 initialize_request(),
                 json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}},
                 initialized_notification(),
                 json{{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/list"}}});

  const json* before = response(messages, 1);
  const json* initialized = response(messages, "initialize");
  const json* awaiting = response(messages, 2);
  const json* ready = response(messages, 3);
  ASSERT_NE(before, nullptr);
  ASSERT_NE(initialized, nullptr);
  ASSERT_NE(awaiting, nullptr);
  ASSERT_NE(ready, nullptr);
  EXPECT_EQ((*before)["error"]["code"], -32002);
  EXPECT_EQ((*awaiting)["error"]["code"], -32002);
  EXPECT_EQ((*initialized)["result"]["protocolVersion"], "2025-06-18");
  EXPECT_EQ((*initialized)["result"]["serverInfo"]["name"], "libtmux-cxx");
  EXPECT_TRUE((*initialized)["result"]["capabilities"].contains("tools"));
  EXPECT_FALSE((*initialized)["result"].contains("resultType"));
  EXPECT_TRUE((*ready)["result"]["tools"].is_array());
  EXPECT_FALSE((*ready)["result"].contains("resultType"));
}

TEST_F(McpProtocol, PublishesTheTwelveToolPosixCatalog) {
  const auto messages = converse_ready(
      socket(), {json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}});
  const json* listed = response(messages, 1);
  ASSERT_NE(listed, nullptr);
  const auto& tools = (*listed)["result"]["tools"];
  ASSERT_TRUE(tools.is_array());
  ASSERT_EQ(tools.size(), 12U);

  std::vector<std::string> names;
  for (const auto& tool : tools) {
    names.push_back(tool["name"].get<std::string>());
    EXPECT_FALSE(tool["title"].get<std::string>().empty()) << tool["name"];
    EXPECT_FALSE(tool["description"].get<std::string>().empty()) << tool["name"];
    EXPECT_EQ(tool["inputSchema"]["type"], "object");
    EXPECT_FALSE(tool["inputSchema"]["additionalProperties"].get<bool>());
    EXPECT_EQ(tool["outputSchema"]["type"], "object");
    EXPECT_TRUE(tool.contains("annotations"));
    for (const auto& [name, property] : tool["inputSchema"]["properties"].items()) {
      static_cast<void>(name);
      EXPECT_TRUE(property["type"] == "string" || property["type"] == "integer");
      EXPECT_FALSE(property.value("description", "").empty());
    }
  }
  std::ranges::sort(names);
  EXPECT_EQ(names,
            (std::vector<std::string>{
                "capture_pane", "create_session", "inspect_tmux", "list_panes",
                "list_session_panes", "list_sessions", "list_windows", "new_window",
                "search_panes", "send_keys", "send_text", "wait_for_text"}));

  const auto waiting = std::ranges::find(tools, "wait_for_text", [](const json& tool) {
    return tool["name"].get<std::string>();
  });
  ASSERT_NE(waiting, tools.end());
  const auto& schema = (*waiting)["inputSchema"];
  EXPECT_EQ(schema["properties"]["timeout_ms"]["type"], "integer");
  EXPECT_EQ(schema["properties"]["timeout_ms"]["minimum"], 1);
  EXPECT_EQ(schema["properties"]["timeout_ms"]["maximum"], 60000);
  EXPECT_EQ(std::ranges::find(schema["required"], "timeout_ms"),
            schema["required"].end());
}

TEST_F(McpProtocol, ReturnsStructuredAndCompatibleTextContent) {
  const auto messages =
      converse_ready(socket(), {call("list_sessions", json::object(), 1)});
  const json* reply = response(messages, 1);
  ASSERT_NE(reply, nullptr);
  const auto& result = (*reply)["result"];
  ASSERT_FALSE(result["isError"].get<bool>());
  ASSERT_TRUE(result["structuredContent"]["sessions"].is_array());
  ASSERT_EQ(result["content"].size(), 1U);
  EXPECT_EQ(result["content"][0]["type"], "text");
  EXPECT_EQ(result["content"][0]["text"], result["structuredContent"].dump());
  EXPECT_EQ(json::parse(result["content"][0]["text"].get<std::string>()),
            result["structuredContent"]);
  EXPECT_EQ(result["structuredContent"]["sessions"][0]["name"], "mcp");
}

TEST_F(McpProtocol, ReturnsCurrentStructuredAndCompatibleTextContent) {
  const auto messages =
      converse(socket(), {modern_request("server/discover", "discover"),
                          modern_call("list_sessions", json::object(), 1)});
  const json* reply = response(messages, 1);
  ASSERT_NE(reply, nullptr);
  const auto& result = (*reply)["result"];
  EXPECT_EQ(result["resultType"], "complete");
  EXPECT_EQ(result["_meta"]["io.modelcontextprotocol/serverInfo"]["name"],
            "libtmux-cxx");
  ASSERT_EQ(result["content"].size(), 1U);
  EXPECT_EQ(result["content"][0]["text"], result["structuredContent"].dump());
  EXPECT_EQ(result["structuredContent"]["sessions"][0]["name"], "mcp");
}

TEST_F(McpProtocol, SeparatesCallerErrorsFromTmuxRefusals) {
  const auto messages = converse_ready(
      socket(),
      {call("capture_pane", json::object(), 1), call("no_such_tool", json::object(), 2),
       call("capture_pane", {{"target", "%999"}}, 3),
       call("capture_pane", {{"target", "mcp"}, {"typo", "x"}}, 4)});
  const json* missing = response(messages, 1);
  const json* unknown = response(messages, 2);
  const json* refused = response(messages, 3);
  const json* misspelt = response(messages, 4);
  ASSERT_NE(missing, nullptr);
  ASSERT_NE(unknown, nullptr);
  ASSERT_NE(refused, nullptr);
  ASSERT_NE(misspelt, nullptr);
  EXPECT_EQ((*missing)["error"]["code"], -32602);
  EXPECT_EQ((*unknown)["error"]["code"], -32602);
  EXPECT_EQ((*misspelt)["error"]["code"], -32602);
  EXPECT_TRUE((*refused)["result"]["isError"].get<bool>());
  EXPECT_FALSE((*refused)["result"]["content"][0]["text"].get<std::string>().empty());
}

TEST_F(McpProtocol, EnforcesPublishedArgumentTypes) {
  const auto messages = converse_ready(
      socket(),
      {call("capture_pane", {{"target", 999}}, 1),
       call("wait_for_text",
            {{"target", "mcp"}, {"text", "type-check-marker"}, {"timeout_ms", "5"}}, 2),
       call("wait_for_text",
            {{"target", "mcp"}, {"text", "integer-timeout-marker"}, {"timeout_ms", 5}},
            3)});
  const json* target_type = response(messages, 1);
  const json* timeout_type = response(messages, 2);
  const json* valid = response(messages, 3);
  ASSERT_NE(target_type, nullptr);
  ASSERT_NE(timeout_type, nullptr);
  ASSERT_NE(valid, nullptr);
  EXPECT_EQ((*target_type)["error"]["code"], -32602);
  EXPECT_EQ((*timeout_type)["error"]["code"], -32602);
  EXPECT_FALSE((*valid)["result"]["isError"].get<bool>());
  EXPECT_TRUE((*valid)["result"]["structuredContent"]["timed_out"].get<bool>());
}

TEST_F(McpProtocol, CreatesThenDiscoversAWindow) {
  const auto created_messages = converse_ready(
      socket(), {call("new_window", {{"session", "mcp"}, {"name", "from-mcp"}}, 1)});
  const json* created = response(created_messages, 1);
  ASSERT_NE(created, nullptr);
  ASSERT_FALSE((*created)["result"]["isError"].get<bool>());
  const std::string window_id =
      (*created)["result"]["structuredContent"]["window_id"].get<std::string>();
  EXPECT_EQ(window_id.front(), '@');

  const auto typed_messages = converse_ready(
      socket(), {call("send_text", {{"target", window_id}, {"text", "marker"}}, 2)});
  const json* typed = response(typed_messages, 2);
  ASSERT_NE(typed, nullptr);
  EXPECT_FALSE((*typed)["result"]["isError"].get<bool>());

  const auto listed_messages =
      converse_ready(socket(), {call("list_panes", json::object(), 3)});
  const json* listed = response(listed_messages, 3);
  ASSERT_NE(listed, nullptr);
  bool found = false;
  for (const auto& pane : (*listed)["result"]["structuredContent"]["panes"]) {
    found = found || pane["window_id"] == window_id;
  }
  EXPECT_TRUE(found);
}

TEST_F(McpProtocol, ValidatesEveryNamedKeyBeforeSending) {
  const auto messages = converse_ready(
      socket(), {call("send_keys", {{"target", "mcp"}, {"keys", "Enter"}}, 1),
                 call("send_keys", {{"target", "mcp"}, {"keys", "NotAKey"}}, 2)});
  const json* sent = response(messages, 1);
  const json* invalid = response(messages, 2);
  ASSERT_NE(sent, nullptr);
  ASSERT_NE(invalid, nullptr);
  EXPECT_FALSE((*sent)["result"]["isError"].get<bool>());
  EXPECT_EQ((*invalid)["error"]["code"], -32602);
}

TEST_F(McpProtocol, WaitsThroughControlOutputAndSearchesTheResult) {
  // The marker is produced after the wait is already running, so it cannot be
  // on the screen when the tool captures at entry. That is what makes this the
  // streaming path; a delay in the pane only makes it likely, and stops being
  // likely on a machine slow enough to echo before the wait starts.
  const std::vector<json> start_wait{
      initialize_request(), initialized_notification(),
      call("wait_for_text",
           {{"target", "mcp"}, {"text", "mcp-stream-marker"}, {"timeout_ms", 9000}},
           1)};
  const std::vector<json> produce{
      call("send_text",
           {{"target", "mcp"}, {"text", "printf 'mcp-%s\\n' 'stream-marker'\n"}}, 2)};
  const std::vector<json> search{
      call("search_panes", {{"text", "mcp-stream-marker"}}, 3)};

  const auto messages = converse_steps(
      socket(), {{encode_requests(start_wait), std::chrono::milliseconds{1500}},
                 {encode_requests(produce), std::chrono::milliseconds{6000}},
                 {encode_requests(search), std::chrono::milliseconds{3000}}});

  const json* waited = response(messages, 1);
  ASSERT_NE(waited, nullptr);
  ASSERT_FALSE((*waited)["result"]["isError"].get<bool>());
  EXPECT_TRUE((*waited)["result"]["structuredContent"]["matched"].get<bool>());
  EXPECT_EQ((*waited)["result"]["structuredContent"]["mode"], "control-output");

  const json* searched = response(messages, 3);
  ASSERT_NE(searched, nullptr);
  ASSERT_FALSE((*searched)["result"]["isError"].get<bool>());
}

TEST_F(McpProtocol, KeepsPingResponsiveDuringALongWait) {
  const std::vector<json> start_wait{initialize_request(), initialized_notification(),
                                     call("wait_for_text",
                                          {{"target", "mcp"},
                                           {"text", "pipelined-marker-never-appears"},
                                           {"timeout_ms", 3000}},
                                          1, "ping-progress")};
  const std::vector<json> ping{json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "ping"}}};
  const auto messages = converse_steps(
      socket(), {{encode_requests(start_wait), std::chrono::milliseconds{1500}},
                 {encode_requests(ping), std::chrono::milliseconds{2000}}});
  const auto progress_position =
      std::ranges::find_if(messages, [](const json& message) {
        return message.value("method", "") == "notifications/progress" &&
               message["params"]["progressToken"] == "ping-progress";
      });
  const auto wait_position = response_position(messages, 1);
  const auto ping_position = response_position(messages, 2);
  ASSERT_NE(progress_position, messages.end());
  ASSERT_TRUE(wait_position.has_value());
  ASSERT_TRUE(ping_position.has_value());
  EXPECT_LT(static_cast<std::size_t>(progress_position - messages.begin()),
            *ping_position);
  EXPECT_LT(*ping_position, *wait_position);
  const json* waited = response(messages, 1);
  ASSERT_NE(waited, nullptr);
  EXPECT_TRUE((*waited)["result"]["structuredContent"]["timed_out"].get<bool>());
}

TEST_F(McpProtocol, EmitsProgressForABoundedWait) {
  const auto messages =
      converse_ready(socket(),
                     {call("wait_for_text",
                           {{"target", "mcp"},
                            {"text", "progress-marker-that-never-appears"},
                            {"timeout_ms", 1100}},
                           1, 1.5)},
                     std::chrono::milliseconds{1400});
  const auto progress = std::ranges::find_if(messages, [](const json& message) {
    return message.value("method", "") == "notifications/progress";
  });
  ASSERT_NE(progress, messages.end());
  EXPECT_EQ((*progress)["params"]["progressToken"], 1.5);
  EXPECT_EQ((*progress)["params"]["total"], 1100);
  const json* waited = response(messages, 1);
  ASSERT_NE(waited, nullptr);
  EXPECT_TRUE((*waited)["result"]["structuredContent"]["timed_out"].get<bool>());
}

TEST_F(McpProtocol, CancelsAnInFlightWaitWithoutAReply) {
  const std::vector<json> start_wait{initialize_request(), initialized_notification(),
                                     call("wait_for_text",
                                          {{"target", "mcp"},
                                           {"text", "cancel-marker-that-never-appears"},
                                           {"timeout_ms", 5000}},
                                          1, "cancel-progress")};
  const std::vector<json> cancel_wait{
      json{{"jsonrpc", "2.0"},
           {"method", "notifications/cancelled"},
           {"params", {{"requestId", 1}, {"reason", "test complete"}}}},
      json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "ping"}}};
  const auto messages = converse_steps(
      socket(), {{encode_requests(start_wait), std::chrono::milliseconds{2500}},
                 {encode_requests(cancel_wait), std::chrono::milliseconds{3500}}});
  const auto progress = std::ranges::find_if(messages, [](const json& message) {
    return message.value("method", "") == "notifications/progress" &&
           message["params"]["progressToken"] == "cancel-progress";
  });
  EXPECT_NE(progress, messages.end());
  EXPECT_EQ(response(messages, 1), nullptr);
  EXPECT_NE(response(messages, 2), nullptr);
}

TEST_F(McpProtocol, CancelsOutstandingWorkAtEndOfInput) {
  const auto started = std::chrono::steady_clock::now();
  const std::vector<json> requests{initialize_request(), initialized_notification(),
                                   call("wait_for_text",
                                        {{"target", "mcp"},
                                         {"text", "eof-marker-that-never-appears"},
                                         {"timeout_ms", 60000}},
                                        1, "eof-progress")};
  const auto messages = converse_steps(
      socket(), {{encode_requests(requests), std::chrono::milliseconds{2500}}});
  const auto elapsed = std::chrono::steady_clock::now() - started;
  const auto progress = std::ranges::find_if(messages, [](const json& message) {
    return message.value("method", "") == "notifications/progress" &&
           message["params"]["progressToken"] == "eof-progress";
  });
  EXPECT_NE(progress, messages.end());
  EXPECT_EQ(response(messages, 1), nullptr);
  EXPECT_LT(elapsed, std::chrono::seconds{4});
}

TEST_F(McpProtocol, CancelsAModernCallAfterDiscovery) {
  const std::vector<json> start_wait{
      modern_request("server/discover", "discover"),
      modern_call("wait_for_text",
                  {{"target", "mcp"},
                   {"text", "modern-cancel-marker-that-never-appears"},
                   {"timeout_ms", 5000}},
                  1, "modern-cancel-progress")};
  const std::vector<json> cancel_wait{modern_cancel(1),
                                      modern_request("tools/list", 2)};
  const auto messages = converse_steps(
      socket(), {{encode_requests(start_wait), std::chrono::milliseconds{2500}},
                 {encode_requests(cancel_wait), std::chrono::milliseconds{3500}}});
  const auto progress = std::ranges::find_if(messages, [](const json& message) {
    return message.value("method", "") == "notifications/progress" &&
           message["params"]["progressToken"] == "modern-cancel-progress";
  });
  EXPECT_NE(progress, messages.end());
  EXPECT_EQ(response(messages, 1), nullptr);
  const json* listed = response(messages, 2);
  ASSERT_NE(listed, nullptr);
  EXPECT_EQ((*listed)["result"]["resultType"], "complete");
}

TEST_F(McpProtocol, CancelsALongCallInsideALegacyBatch) {
  const json batch =
      json::array({call("wait_for_text",
                        {{"target", "mcp"},
                         {"text", "batch-cancel-marker-that-never-appears"},
                         {"timeout_ms", 5000}},
                        1, "batch-cancel-progress"),
                   json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "ping"}}});
  std::string start =
      encode_requests({initialize_request("2025-03-26"), initialized_notification()});
  start += batch.dump();
  start += '\n';
  const std::string cancel =
      encode_requests({json{{"jsonrpc", "2.0"},
                            {"method", "notifications/cancelled"},
                            {"params", {{"requestId", 1}}}},
                       json{{"jsonrpc", "2.0"}, {"id", 3}, {"method", "ping"}}});
  const auto messages =
      converse_steps(socket(), {{std::move(start), std::chrono::milliseconds{2500}},
                                {cancel, std::chrono::milliseconds{3500}}});
  const auto progress = std::ranges::find_if(messages, [](const json& message) {
    return message.value("method", "") == "notifications/progress" &&
           message["params"]["progressToken"] == "batch-cancel-progress";
  });
  EXPECT_NE(progress, messages.end());
  EXPECT_EQ(response(messages, 1), nullptr);
  EXPECT_NE(response(messages, 2), nullptr);
  EXPECT_NE(response(messages, 3), nullptr);
}

TEST(McpProtocolCli, SupportsModernDiscoveryAndCacheableResults) {
  const auto messages = converse_with(
      {"--socket-name", "libtmux-cxx-mcp-modern-no-dispatch"},
      libtmux::test::current_environment(),
      {modern_request("server/discover", "discover"), modern_cancel(999),
       modern_request("tools/list", "list"), modern_request("ping", "ping")});
  const json* discovered = response(messages, "discover");
  const json* listed = response(messages, "list");
  const json* ping = response(messages, "ping");
  ASSERT_NE(discovered, nullptr);
  ASSERT_NE(listed, nullptr);
  ASSERT_NE(ping, nullptr);

  const auto& discovery = (*discovered)["result"];
  EXPECT_EQ(discovery["resultType"], "complete");
  EXPECT_EQ(discovery["supportedVersions"], modern_versions());
  EXPECT_TRUE(discovery["capabilities"].contains("tools"));
  EXPECT_EQ(discovery["ttlMs"], 3600000);
  EXPECT_EQ(discovery["cacheScope"], "public");
  EXPECT_EQ(discovery["_meta"]["io.modelcontextprotocol/serverInfo"]["name"],
            "libtmux-cxx");

  const auto& catalog = (*listed)["result"];
  EXPECT_EQ(catalog["resultType"], "complete");
  EXPECT_EQ(catalog["ttlMs"], 3600000);
  EXPECT_EQ(catalog["cacheScope"], "public");
  EXPECT_EQ(catalog["tools"].size(), 12U);
  EXPECT_EQ(catalog["_meta"]["io.modelcontextprotocol/serverInfo"]["name"],
            "libtmux-cxx");
  EXPECT_EQ((*ping)["error"]["code"], -32601);
}

TEST(McpProtocolCli, AggregatesMixedLegacyBatchMembersInInputOrder) {
  const json batch = json::array(
      {call(std::string(256U * 1024U, 'x'), json::object(), 1), modern_cancel(999),
       json{{"jsonrpc", "2.0"}, {"method", "tools/list"}, {"params", json::array()}},
       17, json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "ping"}},
       json{{"foo", "boo"}}, json{{"jsonrpc", "2.0"}, {"id", 4}, {"foo", "boo"}}});
  const auto messages = converse_batch(batch);
  ASSERT_EQ(messages.size(), 2U);
  ASSERT_TRUE(messages[1].is_array());
  const json& replies = messages[1];
  ASSERT_EQ(replies.size(), 5U) << replies.dump();
  EXPECT_EQ(replies[0]["id"], 1);
  EXPECT_TRUE(replies[0].contains("error"));
  EXPECT_TRUE(replies[1]["id"].is_null());
  EXPECT_EQ(replies[1]["error"]["code"], -32600);
  EXPECT_EQ(replies[2]["id"], 2);
  EXPECT_TRUE(replies[2].contains("result"));
  EXPECT_TRUE(replies[3]["id"].is_null());
  EXPECT_EQ(replies[3]["error"]["code"], -32600);
  EXPECT_EQ(replies[4]["id"], 4);
  EXPECT_EQ(replies[4]["error"]["code"], -32600);
}

TEST(McpProtocolCli, EmitsNothingForAnAllNotificationBatch) {
  const json batch = json::array(
      {modern_cancel(999),
       json{{"jsonrpc", "2.0"}, {"method", "tools/list"}, {"params", json::array()}},
       json{{"jsonrpc", "2.0"},
            {"method", "notifications/unknown"},
            {"params", json::object()}}});
  const auto messages = converse_batch(batch);
  ASSERT_EQ(messages.size(), 1U);
  EXPECT_EQ(messages[0]["id"], "initialize");
}

TEST(McpProtocolCli, NeverRepliesToAMethodInvalidNotification) {
  const auto messages = converse_with(
      {"--socket-name", "libtmux-cxx-mcp-invalid-notification-no-dispatch"},
      libtmux::test::current_environment(),
      {json{{"jsonrpc", "2.0"}, {"method", "tools/list"}, {"params", json::array()}},
       modern_request("tools/list", 1)});
  ASSERT_EQ(messages.size(), 1U);
  EXPECT_EQ(messages[0]["id"], 1);
}

TEST(McpProtocolCli, IgnoresInboundResponsesWithoutPoisoningIds) {
  const json inbound_result{{"jsonrpc", "2.0"}, {"id", 7}, {"result", json::object()}};
  const json inbound_error{{"jsonrpc", "2.0"},
                           {"id", 8},
                           {"error", {{"code", -32000}, {"message", "peer error"}}}};
  const auto messages =
      converse_with({"--socket-name", "libtmux-cxx-mcp-inbound-response-no-dispatch"},
                    libtmux::test::current_environment(),
                    {inbound_result, inbound_error, modern_request("tools/list", 7),
                     modern_request("tools/list", 8)});
  ASSERT_EQ(messages.size(), 2U);
  EXPECT_NE(response(messages, 7), nullptr);
  EXPECT_NE(response(messages, 8), nullptr);
}

TEST(McpProtocolCli, OmitsInboundResponsesFromLegacyBatches) {
  const json batch =
      json::array({json{{"jsonrpc", "2.0"}, {"id", 7}, {"result", json::object()}},
                   json{{"jsonrpc", "2.0"},
                        {"id", 8},
                        {"error", {{"code", -32000}, {"message", "peer error"}}}},
                   json{{"jsonrpc", "2.0"}, {"id", 7}, {"method", "ping"}}});
  const auto messages = converse_batch(batch);
  ASSERT_EQ(messages.size(), 2U);
  ASSERT_TRUE(messages[1].is_array());
  ASSERT_EQ(messages[1].size(), 1U);
  EXPECT_EQ(messages[1][0]["id"], 7);
  EXPECT_TRUE(messages[1][0].contains("result"));
}

TEST(McpProtocolCli, AppliesJsonRpcEmptyAndInvalidBatchSemantics) {
  const auto empty = converse_batch(json::array());
  ASSERT_EQ(empty.size(), 2U);
  ASSERT_TRUE(empty[1].is_object());
  EXPECT_TRUE(empty[1]["id"].is_null());
  EXPECT_EQ(empty[1]["error"]["code"], -32600);

  const auto invalid = converse_batch(json::array({1}));
  ASSERT_EQ(invalid.size(), 2U);
  ASSERT_TRUE(invalid[1].is_array());
  ASSERT_EQ(invalid[1].size(), 1U);
  EXPECT_TRUE(invalid[1][0]["id"].is_null());
  EXPECT_EQ(invalid[1][0]["error"]["code"], -32600);
}

TEST(McpProtocolCli, RejectsBatchesOutsideThe2025MarchOperationPhase) {
  const auto fresh =
      converse_raw("libtmux-cxx-mcp-batch-fresh-no-dispatch",
                   json::array({initialize_request("2025-03-26")}).dump() + '\n');
  ASSERT_EQ(fresh.size(), 1U);
  EXPECT_TRUE(fresh[0].is_object());
  EXPECT_EQ(fresh[0]["error"]["code"], -32600);

  std::string awaiting = initialize_request("2025-03-26").dump() + '\n';
  awaiting += json::array({initialized_notification()}).dump() + '\n';
  const auto awaiting_messages =
      converse_raw("libtmux-cxx-mcp-batch-awaiting-no-dispatch", std::move(awaiting));
  ASSERT_EQ(awaiting_messages.size(), 2U);
  EXPECT_EQ(awaiting_messages[0]["id"], "initialize");
  EXPECT_TRUE(awaiting_messages[1].is_object());
  EXPECT_EQ(awaiting_messages[1]["error"]["code"], -32600);

  for (const std::string_view version : {"2024-11-05", "2025-06-18", "2025-11-25"}) {
    std::string input =
        encode_requests({initialize_request(version), initialized_notification()});
    input +=
        json::array({json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}}}).dump();
    input += '\n';
    const auto messages =
        converse_raw("libtmux-cxx-mcp-batch-version-no-dispatch", std::move(input));
    ASSERT_EQ(messages.size(), 2U) << version;
    EXPECT_TRUE(messages[1].is_object()) << version;
    EXPECT_EQ(messages[1]["error"]["code"], -32600) << version;
  }

  std::string modern = encode_requests({modern_request("server/discover", "discover")});
  modern += json::array({modern_request("tools/list", 1)}).dump();
  modern += '\n';
  const auto messages =
      converse_raw("libtmux-cxx-mcp-modern-batch-no-dispatch", std::move(modern));
  ASSERT_EQ(messages.size(), 2U);
  EXPECT_TRUE(messages[1].is_object());
  EXPECT_EQ(messages[1]["error"]["code"], -32600);
}

TEST(McpProtocolCli, KeepsBatchIdsReservedAndLegacyIdsUnique) {
  const json duplicate =
      json::array({json{{"jsonrpc", "2.0"}, {"id", 7}, {"method", "ping"}},
                   json{{"jsonrpc", "2.0"}, {"id", 7}, {"method", "ping"}},
                   json{{"jsonrpc", "2.0"}, {"id", "initialize"}, {"method", "ping"}}});
  std::string duplicate_input =
      encode_requests({initialize_request("2025-03-26"), initialized_notification()});
  duplicate_input += duplicate.dump() + '\n';
  duplicate_input +=
      json{{"jsonrpc", "2.0"}, {"id", 7}, {"method", "ping"}}.dump() + '\n';
  const auto duplicate_messages = converse_raw(
      "libtmux-cxx-mcp-batch-duplicate-no-dispatch", std::move(duplicate_input));
  ASSERT_EQ(duplicate_messages.size(), 3U);
  ASSERT_TRUE(duplicate_messages[1].is_array());
  ASSERT_EQ(duplicate_messages[1].size(), 3U);
  EXPECT_EQ(duplicate_messages[1][0]["error"]["message"],
            "duplicate request id in batch");
  EXPECT_EQ(duplicate_messages[1][1]["error"]["message"],
            "duplicate request id in batch");
  EXPECT_EQ(duplicate_messages[1][2]["error"]["message"],
            "request id was already used");
  EXPECT_EQ(duplicate_messages[2]["error"]["message"], "request id was already used");

  std::string reused =
      encode_requests({initialize_request("2025-03-26"), initialized_notification()});
  reused +=
      json::array({json{{"jsonrpc", "2.0"}, {"id", 8}, {"method", "ping"}}}).dump();
  reused += '\n';
  reused += json{{"jsonrpc", "2.0"}, {"id", 8}, {"method", "ping"}}.dump();
  reused += '\n';
  const auto reused_messages =
      converse_raw("libtmux-cxx-mcp-batch-reuse-no-dispatch", std::move(reused));
  ASSERT_EQ(reused_messages.size(), 3U);
  EXPECT_EQ(reused_messages[2]["error"]["message"], "request id was already used");
}

TEST_F(McpProtocol, RejectsEveryDuplicateBeforeBatchDispatch) {
  const std::string marker = "mcp-duplicate-preflight-marker";
  const json batch =
      json::array({call("send_text", {{"target", "mcp"}, {"text", marker}}, 7),
                   call("send_text", {{"target", "mcp"}, {"text", marker}}, 7)});
  const auto messages =
      converse(socket(), {initialize_request("2025-03-26"), initialized_notification(),
                          batch, call("capture_pane", {{"target", "mcp"}}, 8)});
  const json* first = response(messages, 7);
  const json* captured = response(messages, 8);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(captured, nullptr);
  ASSERT_TRUE(messages[1].is_array());
  ASSERT_EQ(messages[1].size(), 2U);
  EXPECT_EQ(messages[1][0]["error"]["message"], "duplicate request id in batch");
  EXPECT_EQ(messages[1][1]["error"]["message"], "duplicate request id in batch");
  EXPECT_EQ((*captured)["result"]["structuredContent"]["text"].get<std::string>().find(
                marker),
            std::string::npos);
}

TEST(McpProtocolCli, RejectsFractionalRequestIds) {
  const auto messages =
      converse_with({"--socket-name", "libtmux-cxx-mcp-fractional-id-no-dispatch"},
                    libtmux::test::current_environment(),
                    {json{{"jsonrpc", "2.0"}, {"id", 1.5}, {"method", "initialize"}}});
  ASSERT_EQ(messages.size(), 1U);
  EXPECT_TRUE(messages[0]["id"].is_null());
  EXPECT_EQ(messages[0]["error"]["code"], -32600);
}

TEST(McpProtocolCli, RecoversAfterMalformedJson) {
  const std::string input =
      "{not-json}\n" + modern_request("tools/list", 1).dump() + '\n';
  auto environment = libtmux::test::current_environment();
#if defined(LIBTMUX_MCP_LIBCXX_EXCEPTION_WORKAROUND)
  bool found_asan_options = false;
  for (std::string& entry : environment) {
    if (entry.starts_with("ASAN_OPTIONS=")) {
      entry += ":alloc_dealloc_mismatch=0";
      found_asan_options = true;
      break;
    }
  }
  if (!found_asan_options) {
    environment.emplace_back("ASAN_OPTIONS=alloc_dealloc_mismatch=0");
  }
#endif
  const auto finished = libtmux::mcp::test::run_server(
      LIBTMUX_MCP_SERVER_PATH, {"--socket-name", "libtmux-cxx-mcp-parse-no-dispatch"},
      std::move(environment), input, std::chrono::seconds{60},
      std::chrono::milliseconds{250});
  const auto messages = decode_messages(finished);
  ASSERT_EQ(messages.size(), 2U);
  EXPECT_EQ(messages[0]["error"]["code"], -32700);
  EXPECT_EQ(messages[0]["error"]["message"], "Parse error");
  const json* recovered = response(messages, 1);
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ((*recovered)["result"]["resultType"], "complete");
}

TEST(McpProtocolCli, DrainsAnOversizedFrameBeforeTheNextRequest) {
  constexpr std::size_t maximum_line_bytes = 8U * 1024U * 1024U;
  std::string input(maximum_line_bytes + 1U, 'x');
  input += '\n';
  input += modern_request("tools/list", 1).dump();
  input += '\n';
  const auto messages =
      converse_raw("libtmux-cxx-mcp-frame-no-dispatch", std::move(input));
  ASSERT_EQ(messages.size(), 2U);
  EXPECT_EQ(messages[0]["error"]["code"], -32600);
  EXPECT_EQ(messages[0]["error"]["message"], "request line too long");
  const json* recovered = response(messages, 1);
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ((*recovered)["result"]["resultType"], "complete");
}

TEST(McpProtocolCli, NegotiatesLegacyAndRejectsUnknownModernVersions) {
  json legacy = initialize_request();
  legacy["params"]["protocolVersion"] = "1900-01-01";
  const auto legacy_messages = converse_with(
      {"--socket-name", "libtmux-cxx-mcp-legacy-version-no-dispatch"},
      libtmux::test::current_environment(),
      {legacy, initialized_notification(),
       json{{"jsonrpc", "2.0"}, {"id", "list"}, {"method", "tools/list"}}});
  const json* legacy_reply = response(legacy_messages, "initialize");
  const json* negotiated_list = response(legacy_messages, "list");
  ASSERT_NE(legacy_reply, nullptr);
  ASSERT_NE(negotiated_list, nullptr);
  EXPECT_EQ((*legacy_reply)["result"]["protocolVersion"], "2025-11-25");
  EXPECT_TRUE((*negotiated_list)["result"]["tools"].is_array());

  json modern = modern_request("server/discover", "discover");
  modern["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] = "1900-01-01";
  const auto modern_messages = converse_with(
      {"--socket-name", "libtmux-cxx-mcp-modern-version-no-dispatch"},
      libtmux::test::current_environment(),
      {modern, initialize_request(), initialized_notification(),
       json{{"jsonrpc", "2.0"}, {"id", "legacy-list"}, {"method", "tools/list"}}});
  const json* modern_reply = response(modern_messages, "discover");
  const json* fallback_initialized = response(modern_messages, "initialize");
  const json* fallback_list = response(modern_messages, "legacy-list");
  ASSERT_NE(modern_reply, nullptr);
  ASSERT_NE(fallback_initialized, nullptr);
  ASSERT_NE(fallback_list, nullptr);
  EXPECT_EQ((*modern_reply)["error"]["code"], -32022);
  EXPECT_EQ((*modern_reply)["error"]["data"]["requested"], "1900-01-01");
  EXPECT_EQ((*modern_reply)["error"]["data"]["supported"], modern_versions());
  EXPECT_EQ((*fallback_initialized)["result"]["protocolVersion"], "2025-06-18");
  EXPECT_TRUE((*fallback_list)["result"]["tools"].is_array());
}

TEST(McpProtocolCli, EchoesEverySupportedLegacyVersion) {
  for (const std::string_view version :
       {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"}) {
    const auto messages = converse_with(
        {"--socket-name", "libtmux-cxx-mcp-legacy-echo-no-dispatch"},
        libtmux::test::current_environment(), {initialize_request(version)});
    const json* reply = response(messages, "initialize");
    ASSERT_NE(reply, nullptr) << version;
    EXPECT_EQ((*reply)["result"]["protocolVersion"], version) << version;
  }
}

TEST(McpProtocolCli, RejectsInitializeOnlyVersionsInModernMetadata) {
  json request = modern_request("server/discover", "discover");
  request["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] = "2025-11-25";
  const auto messages = converse_with(
      {"--socket-name", "libtmux-cxx-mcp-modern-legacy-version-no-dispatch"},
      libtmux::test::current_environment(), {request});
  const json* reply = response(messages, "discover");
  ASSERT_NE(reply, nullptr);
  EXPECT_EQ((*reply)["error"]["code"], -32022);
  EXPECT_EQ((*reply)["error"]["data"]["requested"], "2025-11-25");
  EXPECT_EQ((*reply)["error"]["data"]["supported"], modern_versions());
}

TEST(McpProtocolCli, TreatsAnyReservedRequestMetadataAsModern) {
  const json partial{
      {"jsonrpc", "2.0"},
      {"id", 1},
      {"method", "tools/list"},
      {"params",
       {{"_meta", {{"io.modelcontextprotocol/clientCapabilities", json::object()}}}}}};
  const auto fresh =
      converse_with({"--socket-name", "libtmux-cxx-mcp-partial-modern-no-dispatch"},
                    libtmux::test::current_environment(), {partial});
  const json* fresh_reply = response(fresh, 1);
  ASSERT_NE(fresh_reply, nullptr);
  EXPECT_EQ((*fresh_reply)["error"]["code"], -32602);

  const auto legacy =
      converse_with({"--socket-name", "libtmux-cxx-mcp-partial-mixed-no-dispatch"},
                    libtmux::test::current_environment(),
                    {initialize_request(), initialized_notification(), partial});
  const json* mixed_reply = response(legacy, 1);
  ASSERT_NE(mixed_reply, nullptr);
  EXPECT_EQ((*mixed_reply)["error"]["code"], -32600);
}

TEST(McpProtocolCli, RemovesInitializeFromTheModernMethodSet) {
  json request = initialize_request();
  request["params"]["_meta"] = modern_metadata();
  const auto messages =
      converse_with({"--socket-name", "libtmux-cxx-mcp-modern-initialize-no-dispatch"},
                    libtmux::test::current_environment(), {request});
  const json* reply = response(messages, "initialize");
  ASSERT_NE(reply, nullptr);
  EXPECT_EQ((*reply)["error"]["code"], -32601);
}

TEST(McpProtocolCli, RejectsMixedEraConversations) {
  const auto legacy_first =
      converse_with({"--socket-name", "libtmux-cxx-mcp-mixed-legacy-no-dispatch"},
                    libtmux::test::current_environment(),
                    {initialize_request(), initialized_notification(),
                     modern_request("tools/list", 1)});
  const json* modern_after_legacy = response(legacy_first, 1);
  ASSERT_NE(modern_after_legacy, nullptr);
  EXPECT_EQ((*modern_after_legacy)["error"]["code"], -32600);

  const auto modern_first = converse_with(
      {"--socket-name", "libtmux-cxx-mcp-mixed-modern-no-dispatch"},
      libtmux::test::current_environment(),
      {modern_request("server/discover", "discover"), initialize_request()});
  const json* legacy_after_modern = response(modern_first, "initialize");
  ASSERT_NE(legacy_after_modern, nullptr);
  EXPECT_EQ((*legacy_after_modern)["error"]["code"], -32600);
}

TEST(McpProtocolCli, ValidatesModernMetadataOnEveryRequest) {
  json invalid_capabilities = modern_request("tools/list", 2);
  invalid_capabilities["params"]["_meta"]
                      ["io.modelcontextprotocol/clientCapabilities"] = json::array();
  json invalid_extension = modern_request("tools/list", 3);
  invalid_extension["params"]["_meta"]["io.modelcontextprotocol/clientCapabilities"] = {
      {"extensions", {{"unprefixed", json::object()}}}};
  const auto messages = converse_with(
      {"--socket-name", "libtmux-cxx-mcp-modern-meta-no-dispatch"},
      libtmux::test::current_environment(),
      {modern_request("server/discover", "discover"),
       json{{"jsonrpc", "2.0"},
            {"id", 1},
            {"method", "tools/list"},
            {"params", json::object()}},
       invalid_capabilities, invalid_extension, modern_request("tools/list", 4)});
  for (const int id : {1, 2, 3}) {
    const json* reply = response(messages, id);
    ASSERT_NE(reply, nullptr);
    EXPECT_EQ((*reply)["error"]["code"], -32602);
  }
  const json* recovered = response(messages, 4);
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ((*recovered)["result"]["resultType"], "complete");
}

TEST(McpProtocolCli, ReportsModernToolInputErrorsAsToolResults) {
  const auto messages = converse_with(
      {"--socket-name", "libtmux-cxx-mcp-modern-input-no-dispatch"},
      libtmux::test::current_environment(),
      {modern_call("capture_pane", {{"target", 99}}, 1),
       modern_call("capture_pane", json::object(), 2),
       modern_request("tools/call", 3,
                      {{"name", "capture_pane"}, {"arguments", json::array()}}),
       modern_call("no_such_tool", json::object(), 4),
       modern_request("tools/call", 5,
                      {{"name", "capture_pane"},
                       {"arguments", json::object()},
                       {"inputResponses", json::object()}}),
       modern_request("tools/call", 6,
                      {{"name", "capture_pane"},
                       {"arguments", json::object()},
                       {"requestState", "unknown"}})});
  for (const int id : {1, 2}) {
    const json* reply = response(messages, id);
    ASSERT_NE(reply, nullptr);
    EXPECT_FALSE(reply->contains("error"));
    EXPECT_EQ((*reply)["result"]["resultType"], "complete");
    EXPECT_TRUE((*reply)["result"]["isError"].get<bool>());
    EXPECT_FALSE((*reply)["result"]["content"][0]["text"].get<std::string>().empty());
  }
  const json* malformed = response(messages, 3);
  const json* unknown = response(messages, 4);
  ASSERT_NE(malformed, nullptr);
  ASSERT_NE(unknown, nullptr);
  EXPECT_EQ((*malformed)["error"]["code"], -32602);
  EXPECT_EQ((*unknown)["error"]["code"], -32602);
  for (const int id : {5, 6}) {
    const json* retry = response(messages, id);
    ASSERT_NE(retry, nullptr);
    EXPECT_EQ((*retry)["error"]["code"], -32602);
    EXPECT_EQ((*retry)["error"]["message"], "this server has no pending input round");
  }
}

TEST(McpProtocolCli, Reports2025NovemberInputErrorsAsToolResults) {
  const auto messages =
      converse_with({"--socket-name", "libtmux-cxx-mcp-november-input-no-dispatch"},
                    libtmux::test::current_environment(),
                    {initialize_request("2025-11-25"), initialized_notification(),
                     call("capture_pane", {{"target", 99}}, 1),
                     call("capture_pane", json::object(), 2),
                     call("no_such_tool", json::object(), 3)});
  for (const int id : {1, 2}) {
    const json* reply = response(messages, id);
    ASSERT_NE(reply, nullptr);
    EXPECT_FALSE(reply->contains("error"));
    EXPECT_FALSE((*reply)["result"].contains("resultType"));
    EXPECT_TRUE((*reply)["result"]["isError"].get<bool>());
  }
  const json* unknown = response(messages, 3);
  ASSERT_NE(unknown, nullptr);
  EXPECT_EQ((*unknown)["error"]["code"], -32602);
}

TEST(McpProtocolCli, ReportsOlderLegacyInputErrorsAsInvalidParams) {
  for (const std::string_view version : {"2025-06-18", "2025-03-26", "2024-11-05"}) {
    const auto messages =
        converse_with({"--socket-name", "libtmux-cxx-mcp-old-input-no-dispatch"},
                      libtmux::test::current_environment(),
                      {initialize_request(version), initialized_notification(),
                       call("capture_pane", json::object(), 1)});
    const json* reply = response(messages, 1);
    ASSERT_NE(reply, nullptr) << version;
    EXPECT_EQ((*reply)["error"]["code"], -32602) << version;
  }
}

TEST(McpProtocolCli, ValidatesLegacyMetadataContainers) {
  json malformed_initialize = initialize_request();
  malformed_initialize["params"]["_meta"] = json::array();
  json valid_initialize = initialize_request();
  valid_initialize["id"] = "initialize-2";
  const auto initialize_messages = converse_with(
      {"--socket-name", "libtmux-cxx-mcp-legacy-init-meta-no-dispatch"},
      libtmux::test::current_environment(), {malformed_initialize, valid_initialize});
  const json* rejected_initialize = response(initialize_messages, "initialize");
  const json* accepted_initialize = response(initialize_messages, "initialize-2");
  ASSERT_NE(rejected_initialize, nullptr);
  ASSERT_NE(accepted_initialize, nullptr);
  EXPECT_EQ((*rejected_initialize)["error"]["code"], -32602);
  EXPECT_EQ((*accepted_initialize)["result"]["protocolVersion"], "2025-06-18");

  json malformed_initialized = initialized_notification();
  malformed_initialized["params"]["_meta"] = json::array();
  const json malformed_list{{"jsonrpc", "2.0"},
                            {"id", 2},
                            {"method", "tools/list"},
                            {"params", {{"_meta", json::array()}}}};
  const auto request_messages = converse_with(
      {"--socket-name", "libtmux-cxx-mcp-legacy-request-meta-no-dispatch"},
      libtmux::test::current_environment(),
      {initialize_request(), malformed_initialized,
       json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}},
       initialized_notification(), malformed_list,
       json{{"jsonrpc", "2.0"},
            {"id", 3},
            {"method", "ping"},
            {"params", {{"_meta", json::array()}}}}});
  const json* still_awaiting = response(request_messages, 1);
  const json* rejected_list = response(request_messages, 2);
  const json* rejected_ping = response(request_messages, 3);
  ASSERT_NE(still_awaiting, nullptr);
  ASSERT_NE(rejected_list, nullptr);
  ASSERT_NE(rejected_ping, nullptr);
  EXPECT_EQ((*still_awaiting)["error"]["code"], -32002);
  EXPECT_EQ((*rejected_list)["error"]["code"], -32602);
  EXPECT_EQ((*rejected_ping)["error"]["code"], -32602);
}

TEST(McpProtocolCli, AcceptsLegacyToolMetadataAndRetainsArguments) {
  json compatible = call("capture_pane", {{"target", 99}}, 1, "client-progress");
  compatible["params"]["_meta"]["claudeCode"] = {{"version", "2.1.234"}};
  json invalid_key = compatible;
  invalid_key["id"] = 2;
  invalid_key["params"]["_meta"]["not a metadata key"] = true;
  const auto messages = converse_with(
      {"--socket-name", "libtmux-cxx-mcp-legacy-call-meta-no-dispatch"},
      libtmux::test::current_environment(),
      {initialize_request(), initialized_notification(), compatible, invalid_key});
  const json* accepted = response(messages, 1);
  const json* rejected = response(messages, 2);
  ASSERT_NE(accepted, nullptr);
  ASSERT_NE(rejected, nullptr);
  EXPECT_EQ((*accepted)["error"]["code"], -32602);
  EXPECT_EQ((*accepted)["error"]["message"], "argument target must be a string");
  EXPECT_EQ((*rejected)["error"]["code"], -32602);
  EXPECT_EQ((*rejected)["error"]["message"], "tools/call _meta is invalid");
}

TEST(McpProtocolTmux, SelectsAnIsolatedServerBySocketName) {
  auto started = ScopedTmuxServer::start(ScopedTmuxServerOptions{
      .mode = SocketMode::Name,
      .session_name = "mcp-name",
      .socket_namespace = SocketNamespace::consumer("mcp-name")});
  ASSERT_TRUE(started.has_value()) << started.error();
  ASSERT_TRUE(started->socket_name().has_value());
  const std::vector<json> requests{initialize_request(), initialized_notification(),
                                   call("list_sessions", json::object(), 1)};
  const auto messages =
      converse_with({"--socket-name", std::string{*started->socket_name()}},
                    started->child_environment(), requests);
  const json* listed = response(messages, 1);
  ASSERT_NE(listed, nullptr);
  EXPECT_EQ((*listed)["result"]["structuredContent"]["sessions"][0]["name"],
            "mcp-name");
}

TEST(McpProtocolTmux, UsesAnExactInheritedRoute) {
  auto started = ScopedTmuxServer::start(ScopedTmuxServerOptions{
      .session_name = "mcp-env",
      .socket_namespace = SocketNamespace::consumer("mcp-env")});
  ASSERT_TRUE(started.has_value()) << started.error();
  auto environment = started->child_environment();
  libtmux::test::set_environment(environment, "TMUX",
                                 started->socket_path().string() + ',' +
                                     std::to_string(started->server_pid()) + ",0");
  const std::vector<json> requests{initialize_request(), initialized_notification(),
                                   call("list_sessions", json::object(), 1)};
  const auto messages = converse_with({}, std::move(environment), requests);
  const json* listed = response(messages, 1);
  ASSERT_NE(listed, nullptr);
  EXPECT_EQ((*listed)["result"]["structuredContent"]["sessions"][0]["name"], "mcp-env");
}

TEST(McpProtocolCli, PublishesCharacterCountLimits) {
  const auto messages =
      converse_with({"--socket-name", "libtmux-cxx-mcp-schema-no-dispatch"},
                    libtmux::test::current_environment(),
                    {initialize_request(), initialized_notification(),
                     json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}});
  const json* listed = response(messages, 1);
  ASSERT_NE(listed, nullptr);
  const auto& tools = (*listed)["result"]["tools"];
  const auto windows = std::ranges::find(tools, "list_windows", [](const json& tool) {
    return tool["name"].get<std::string>();
  });
  const auto waiting = std::ranges::find(tools, "wait_for_text", [](const json& tool) {
    return tool["name"].get<std::string>();
  });
  ASSERT_NE(windows, tools.end());
  ASSERT_NE(waiting, tools.end());
  EXPECT_EQ((*windows)["inputSchema"]["properties"]["session"]["maxLength"], 512);
  EXPECT_EQ((*waiting)["inputSchema"]["properties"]["text"]["maxLength"], 4096);
}

TEST(McpProtocolCli, KeepsAnIdReservedUntilItsReplyIsWritten) {
  const std::string huge_name(2U * 1024U * 1024U, 'x');
  const std::string initialize = initialize_request().dump() + '\n';
  const std::string initialized = initialized_notification().dump() + '\n';
  const std::string first = call(huge_name, json::object(), 7).dump() + '\n';
  const std::string duplicate = call("also_unknown", json::object(), 7).dump() + '\n';
  auto replies = libtmux::mcp::test::run_backpressure_probe(
      LIBTMUX_MCP_SERVER_PATH,
      {"--socket-name", "libtmux-cxx-mcp-backpressure-no-dispatch"},
      libtmux::test::current_environment(), initialize, initialized, first, duplicate,
      std::chrono::seconds{10});
  ASSERT_TRUE(replies.has_value()) << replies.error();
  ASSERT_EQ(replies->size(), 3U);

  const json reply_a = json::parse((*replies)[1]);
  const json reply_b = json::parse((*replies)[2]);
  const auto is_duplicate = [](const json& reply) {
    return reply["error"]["message"] == "request id is already in flight";
  };
  const json& first_reply = is_duplicate(reply_a) ? reply_b : reply_a;
  const json& duplicate_reply = is_duplicate(reply_a) ? reply_a : reply_b;
  EXPECT_EQ(first_reply["id"], 7);
  EXPECT_EQ(duplicate_reply["id"], 7);
  EXPECT_TRUE(
      first_reply["error"]["message"].get<std::string>().starts_with("unknown tool: "));
  EXPECT_TRUE(is_duplicate(duplicate_reply));
}

TEST(McpProtocolCli, KeepsBatchIdsReservedThroughAggregateBackpressure) {
  const std::string huge_name(2U * 1024U * 1024U, 'x');
  const std::string initialize = initialize_request("2025-03-26").dump() + '\n';
  const std::string initialized = initialized_notification().dump() + '\n';
  const std::string first =
      json::array({call(huge_name, json::object(), 9)}).dump() + '\n';
  const std::string duplicate =
      json{{"jsonrpc", "2.0"}, {"id", 9}, {"method", "ping"}}.dump() + '\n';
  auto replies = libtmux::mcp::test::run_backpressure_probe(
      LIBTMUX_MCP_SERVER_PATH,
      {"--socket-name", "libtmux-cxx-mcp-batch-backpressure-no-dispatch"},
      libtmux::test::current_environment(), initialize, initialized, first, duplicate,
      std::chrono::seconds{10});
  ASSERT_TRUE(replies.has_value()) << replies.error();
  ASSERT_EQ(replies->size(), 3U);

  const json reply_a = json::parse((*replies)[1]);
  const json reply_b = json::parse((*replies)[2]);
  const json& aggregate = reply_a.is_array() ? reply_a : reply_b;
  const json& duplicate_reply = reply_a.is_array() ? reply_b : reply_a;
  ASSERT_TRUE(aggregate.is_array());
  ASSERT_EQ(aggregate.size(), 1U);
  EXPECT_EQ(aggregate[0]["id"], 9);
  EXPECT_EQ(duplicate_reply["id"], 9);
  EXPECT_EQ(duplicate_reply["error"]["message"], "request id is already in flight");
}

TEST(McpProtocolCli, RefusesAnImplicitDefaultRoute) {
  auto environment = libtmux::test::current_environment();
  libtmux::test::erase_environment(environment, "TMUX");
  const auto finished = libtmux::mcp::test::run_server(
      LIBTMUX_MCP_SERVER_PATH, {}, std::move(environment), {}, std::chrono::seconds{5});
  ASSERT_FALSE(finished.has_value());
  EXPECT_NE(finished.error().find("exited with status 1"), std::string::npos);
}

TEST(McpProtocolCli, RefusesAnInvalidInheritedRoute) {
  auto environment = libtmux::test::current_environment();
  libtmux::test::set_environment(environment, "TMUX", "");
  const auto finished = libtmux::mcp::test::run_server(
      LIBTMUX_MCP_SERVER_PATH, {}, std::move(environment), {}, std::chrono::seconds{5});
  ASSERT_FALSE(finished.has_value());
  EXPECT_NE(finished.error().find("exited with status 1"), std::string::npos);
}

} // namespace
