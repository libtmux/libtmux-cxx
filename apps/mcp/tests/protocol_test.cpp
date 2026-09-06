// Exercise the installed shape: newline-delimited JSON-RPC over stdio with a
// private real tmux server underneath.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <libtmux/server.hpp>
#include <libtmux/testing/environment_guard.hpp>
#include <libtmux/testing/scoped_server.hpp>

#include <unistd.h>

#include "cli.hpp"
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

std::vector<json> converse_steps(
    const std::filesystem::path& socket,
    const std::vector<libtmux::mcp::test::InputStep>& steps,
    std::vector<std::string> environment = libtmux::test::current_environment()) {
  auto finished = libtmux::mcp::test::run_server_steps(
      LIBTMUX_MCP_SERVER_PATH, {"--socket-path", socket.string()},
      std::move(environment), steps, std::chrono::seconds{60});
  return decode_messages(finished);
}

struct PrivateTmuxEndpointCleanup final {
  const libtmux::Server& server;
  const std::filesystem::path& selected;
  const std::filesystem::path& private_root;

  ~PrivateTmuxEndpointCleanup() noexcept {
    if (selected.parent_path() != private_root) {
      return;
    }
    static_cast<void>(server.kill());
    const auto stopped_by = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (server.is_alive(std::chrono::milliseconds{50}) &&
           std::chrono::steady_clock::now() < stopped_by) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    if (!server.is_alive(std::chrono::milliseconds{50})) {
      std::error_code ignored;
      static_cast<void>(std::filesystem::remove(selected, ignored));
    }
  }
};

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

const json& require_response(const std::vector<json>& messages, const json& id) {
  const json* reply = response(messages, id);
  if (reply == nullptr) {
    throw std::runtime_error{"MCP server did not reply to request " + id.dump()};
  }
  return *reply;
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

std::vector<std::string> buffer_names(const std::vector<libtmux::Buffer>& buffers) {
  std::vector<std::string> names;
  for (const libtmux::Buffer& buffer : buffers) {
    names.emplace_back(buffer.name());
  }
  std::ranges::sort(names);
  return names;
}

std::string shell_quote(std::string_view value) {
  std::string quoted{"'"};
  for (const char character : value) {
    quoted += character == '\'' ? "'\\''" : std::string{character};
  }
  quoted += '\'';
  return quoted;
}

std::optional<std::filesystem::path> executable_on_path(std::string_view name) {
  const char* const configured = std::getenv("PATH");
  if (configured == nullptr) {
    return std::nullopt;
  }
  const std::string_view search{configured};
  std::size_t begin = 0U;
  while (begin <= search.size()) {
    const std::size_t end = search.find(':', begin);
    const std::string_view component = search.substr(
        begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
    const std::filesystem::path directory = component.empty()
                                                ? std::filesystem::current_path()
                                                : std::filesystem::path{component};
    const std::filesystem::path candidate = directory / name;
    std::error_code error;
    if (std::filesystem::is_regular_file(candidate, error) && !error &&
        ::access(candidate.c_str(), X_OK) == 0) {
      const std::filesystem::path canonical =
          std::filesystem::canonical(candidate, error);
      if (!error) {
        return canonical;
      }
    }
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1U;
  }
  return std::nullopt;
}

bool contains_completion_marker(std::string_view text) {
  constexpr std::string_view prefix{"__LIBTMUX_MCP_DONE_"};
  std::size_t begin = text.find(prefix);
  while (begin != std::string_view::npos) {
    const std::size_t nonce = begin + prefix.size();
    if (text.size() >= nonce + 35U &&
        std::ranges::all_of(text.substr(nonce, 32U),
                            [](const char value) {
                              return (value >= '0' && value <= '9') ||
                                     (value >= 'a' && value <= 'f');
                            }) &&
        text.substr(nonce + 32U, 3U) == "__:") {
      return true;
    }
    begin = text.find(prefix, begin + prefix.size());
  }
  return false;
}

std::size_t line_occurrences(std::string_view text, std::string_view expected) {
  std::size_t count = 0U;
  std::size_t begin = 0U;
  while (begin <= text.size()) {
    const std::size_t end = text.find('\n', begin);
    const std::string_view line = text.substr(
        begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
    count += line == expected ? 1U : 0U;
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1U;
  }
  return count;
}

std::vector<std::string> run_shell_trap_files() {
  std::vector<std::string> files;
  std::error_code error;
  const std::filesystem::path temporary = std::filesystem::temp_directory_path(error);
  if (error) {
    return files;
  }
  std::filesystem::directory_iterator entries{temporary, error};
  for (; !error && entries != std::filesystem::directory_iterator{};
       entries.increment(error)) {
    const std::string name = entries->path().filename().string();
    if (name.starts_with("libtmux-mcp-traps-")) {
      files.push_back(name);
    }
  }
  std::ranges::sort(files);
  return files;
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

  [[nodiscard]] libtmux::Server connect_server() const {
    auto connected = libtmux::Server::at_socket_path(socket().string());
    EXPECT_TRUE(connected.has_value()) << connected.error().diagnostic;
    return std::move(*connected);
  }

  [[nodiscard]] json
  invoke(std::string name, json arguments, int id,
         std::chrono::milliseconds linger = std::chrono::milliseconds{250}) const {
    const auto messages =
        converse_ready(socket(), {call(name, std::move(arguments), id)}, linger);
    return require_response(messages, id);
  }

  [[nodiscard]] std::string captured(const libtmux::Pane& pane) const {
    const auto value = pane.capture();
    EXPECT_TRUE(value.has_value()) << value.error().diagnostic;
    return value.has_value() ? *value : std::string{};
  }

  [[nodiscard]] std::vector<libtmux::Pane>
  split_sorted_panes(const libtmux::Server& server) const {
    auto panes = server.panes();
    EXPECT_TRUE(panes.has_value()) << panes.error().diagnostic;
    if (!panes.has_value() || panes->empty()) {
      return {};
    }
    const auto split = panes->front().split();
    EXPECT_TRUE(split.has_value()) << split.error().diagnostic;
    panes = server.panes();
    EXPECT_TRUE(panes.has_value()) << panes.error().diagnostic;
    if (!panes.has_value()) {
      return {};
    }
    std::ranges::sort(*panes, {},
                      [](const libtmux::Pane& pane) { return std::string{pane.id()}; });
    return std::move(*panes);
  }

  [[nodiscard]] libtmux::expected<std::string, libtmux::CommandFailure>
  wait_for_pane_value(const libtmux::Pane& pane, std::string_view format,
                      std::string_view expected) const {
    for (int attempt = 0; attempt < 100; ++attempt) {
      auto value = pane.expand(format);
      if (!value.has_value() || *value == expected) {
        return value;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return pane.expand(format);
  }

  void send_shell_and_wait(const libtmux::Server& server, const libtmux::Pane& pane,
                           const std::filesystem::path& tmux_executable,
                           std::string command) const {
    static std::atomic_uint64_t sequence{0U};
    const std::string channel = "mcp-shell-step-" + std::to_string(++sequence);
    command += "; " + shell_quote(tmux_executable.string()) + " -N -S " +
               shell_quote(socket().string()) + " wait-for -S " + shell_quote(channel);
    ASSERT_TRUE(pane.send_text(command).has_value());
    ASSERT_TRUE(pane.send_key("Enter").has_value());
    const auto ready = server.wait_for(channel, std::chrono::seconds{2});
    ASSERT_TRUE(ready.has_value()) << ready.error().diagnostic;
  }

  [[nodiscard]] std::string
  parent_shell_state(const libtmux::Server& server, const libtmux::Pane& pane,
                     const std::filesystem::path& tmux_executable,
                     std::string_view label) const {
    const std::string begin = "MCP_PARENT_BEGIN_" + std::string{label};
    const std::string end = "MCP_PARENT_END_" + std::string{label};
    const std::string exact_tmux = shell_quote(tmux_executable.string()) + " -N -S " +
                                   shell_quote(socket().string());
    send_shell_and_wait(server, pane, tmux_executable,
                        exact_tmux + " display-message -p " + shell_quote(begin) +
                            "; " + exact_tmux +
                            " display-message -p \"$-|$PWD|${MCP_PARENT_VALUE-}|"
                            "${MCP_PARENT_EXPORT-}\"; \\trap; " +
                            exact_tmux + " display-message -p " + shell_quote(end));
    libtmux::CaptureOptions options;
    options.whole_history = true;
    options.join_wrapped = true;
    const auto capture = pane.capture(options);
    EXPECT_TRUE(capture.has_value()) << capture.error().diagnostic;
    if (!capture.has_value()) {
      return {};
    }
    const std::size_t end_position = capture->rfind(end);
    const std::size_t begin_position = end_position == std::string::npos
                                           ? std::string::npos
                                           : capture->rfind(begin, end_position);
    EXPECT_NE(begin_position, std::string::npos) << *capture;
    EXPECT_NE(end_position, std::string::npos) << *capture;
    if (begin_position == std::string::npos || end_position == std::string::npos) {
      return {};
    }
    std::size_t content = begin_position + begin.size();
    if (content < capture->size() && (*capture)[content] == '\n') {
      ++content;
    }
    const std::string raw = capture->substr(content, end_position - content);
    std::string stable;
    std::size_t line_begin = 0U;
    while (line_begin < raw.size()) {
      const std::size_t line_end = raw.find('\n', line_begin);
      const std::size_t length = line_end == std::string::npos
                                     ? raw.size() - line_begin
                                     : line_end - line_begin + 1U;
      if (raw[line_begin] != '+') {
        stable.append(raw, line_begin, length);
      }
      if (line_end == std::string::npos) {
        break;
      }
      line_begin = line_end + 1U;
    }
    return stable;
  }

  std::unique_ptr<ScopedTmuxServer> fixture_;
};

TEST(McpProtocolHelpers, MissingResponseStopsTheCaller) {
  EXPECT_THROW(static_cast<void>(require_response({}, 7)), std::runtime_error);
}

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

TEST(McpProtocolCli, StartsAnAbsentPinnedSocketOnlyForCreateSession) {
  auto sentinel = ScopedTmuxServer::start(ScopedTmuxServerOptions{
      .mode = SocketMode::Name,
      .session_name = "startable-sentinel",
      .socket_namespace = SocketNamespace::consumer("mcp-new")});
  ASSERT_TRUE(sentinel.has_value()) << sentinel.error();
  const auto private_permissions =
      std::filesystem::status(sentinel->tmux_tmpdir()).permissions();
  EXPECT_EQ(private_permissions & (std::filesystem::perms::group_all |
                                   std::filesystem::perms::others_all),
            std::filesystem::perms::none);
  const libtmux::test::EnvironmentGuard tmux_tmpdir{"TMUX_TMPDIR",
                                                    sentinel->tmux_tmpdir().string()};

  // Short, and no nonce: the fixture's tree is already unique to this run, and
  // a nanosecond count spends nineteen of the bytes `sun_path` has under the
  // macOS-length temporary directory CI runs.
  const std::filesystem::path socket = sentinel->tmux_tmpdir() / "startable.sock";
  EXPECT_EQ(socket.parent_path(), sentinel->tmux_tmpdir());
  EXPECT_FALSE(std::filesystem::exists(socket));
  const std::string initialize =
      encode_requests({initialize_request(), initialized_notification(),
                       call("create_session", {{"name", "mcp-startable"}}, 1)});
  const std::string teardown =
      encode_requests({call("kill_session", {{"session", "mcp-startable"}}, 2)});
  auto environment = libtmux::test::current_environment();
  libtmux::test::erase_environment(environment, "TMUX");
  libtmux::test::erase_environment(environment, "TMUX_PANE");
  libtmux::test::set_environment(environment, "LIBTMUX_TOOLS", "kill_session");
  const auto messages = converse_steps(socket,
                                       {{initialize, std::chrono::milliseconds{750}},
                                        {teardown, std::chrono::milliseconds{750}}},
                                       std::move(environment));
  auto server = libtmux::Server::at_socket_path(socket.string());
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  const json* created = response(messages, 1);
  const json* killed = response(messages, 2);
  EXPECT_THROW(
      [&] {
        PrivateTmuxEndpointCleanup cleanup(*server, socket, sentinel->tmux_tmpdir());
        const auto server_environment = server->run({"show-environment", "-g"});
        ASSERT_TRUE(server_environment.has_value())
            << server_environment.error().diagnostic;
        EXPECT_NE(
            server_environment->find("TMUX_TMPDIR=" + sentinel->tmux_tmpdir().string()),
            std::string::npos);
        ASSERT_NE(created, nullptr);
        ASSERT_NE(killed, nullptr);
        EXPECT_FALSE(created->contains("error")) << created->dump();
        EXPECT_FALSE(killed->contains("error")) << killed->dump();
        EXPECT_FALSE((*created)["result"]["isError"].get<bool>()) << created->dump();
        EXPECT_FALSE((*killed)["result"]["isError"].get<bool>()) << killed->dump();
        throw std::runtime_error{"injected after target creation"};
      }(),
      std::runtime_error);

  EXPECT_FALSE(server->is_alive(std::chrono::milliseconds{50}));
  EXPECT_FALSE(std::filesystem::exists(socket));
  EXPECT_TRUE(sentinel->is_alive());
  const auto sibling =
      libtmux::Server::at_socket_path(sentinel->socket_path().string());
  ASSERT_TRUE(sibling.has_value()) << sibling.error().diagnostic;
  EXPECT_TRUE(sibling->is_alive());
}

TEST_F(McpProtocol, PublishesTheEffectiveCrossPortCatalog) {
  const auto messages = converse_ready(
      socket(), {json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}});
  const json* listed = response(messages, 1);
  ASSERT_NE(listed, nullptr);
  const auto& tools = (*listed)["result"]["tools"];
  ASSERT_TRUE(tools.is_array());
  ASSERT_EQ(tools.size(), 41U);

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
      EXPECT_TRUE(property["type"] == "string" || property["type"] == "integer" ||
                  property["type"] == "boolean" || property["type"] == "array");
      EXPECT_FALSE(property.value("description", "").empty());
    }
  }
  EXPECT_EQ(names, (std::vector<std::string>{"list_sessions",
                                             "list_windows",
                                             "list_panes",
                                             "get_server_info",
                                             "get_session_info",
                                             "get_window_info",
                                             "get_pane_info",
                                             "capture_pane",
                                             "capture_since",
                                             "snapshot_pane",
                                             "search_panes",
                                             "find_pane_by_position",
                                             "wait_for_text",
                                             "get_tmux_variables",
                                             "show_option",
                                             "show_environment",
                                             "show_hooks",
                                             "call_read_tools_batch",
                                             "rename_session",
                                             "rename_window",
                                             "select_window",
                                             "select_pane",
                                             "select_layout",
                                             "resize_window",
                                             "resize_pane",
                                             "move_window",
                                             "swap_pane",
                                             "set_pane_title",
                                             "wait_for_channel",
                                             "signal_channel",
                                             "set_mouse_enabled",
                                             "set_history_limit",
                                             "create_session",
                                             "create_window",
                                             "split_window",
                                             "respawn_pane",
                                             "run_shell_command",
                                             "send_keys",
                                             "send_keys_batch",
                                             "paste_text",
                                             "set_synchronize_panes"}));

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

  const auto batch = std::ranges::find(tools, "send_keys_batch", [](const json& tool) {
    return tool["name"].get<std::string>();
  });
  ASSERT_NE(batch, tools.end());
  const json& operation = (*batch)["inputSchema"]["properties"]["operations"]["items"];
  EXPECT_FALSE(operation["additionalProperties"].get<bool>());
  EXPECT_EQ(operation["required"], json::array({"paneId", "keys"}));
  EXPECT_EQ(operation["properties"].size(), 4U);
  EXPECT_FALSE(operation["properties"].contains("force"));
  EXPECT_EQ(operation["properties"]["paneId"]["maxLength"], 512);
  EXPECT_EQ(operation["properties"]["keys"]["maxLength"], 4096);
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
       call("capture_pane", {{"paneId", "%999"}}, 3),
       call("capture_pane", {{"paneId", "mcp"}, {"typo", "x"}}, 4)});
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
      {call("capture_pane", {{"paneId", 999}}, 1),
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

TEST_F(McpProtocol, EnforcesNestedSendKeysBatchCodePointLimits) {
  const std::string face{"\xF0\x9F\x98\x80"};
  const auto repeated = [&face](std::size_t count) {
    std::string value;
    value.reserve(face.size() * count);
    for (std::size_t index = 0U; index < count; ++index) {
      value += face;
    }
    return value;
  };
  const auto operation = [](std::string pane_id, std::string keys) {
    return json::array({{{"paneId", std::move(pane_id)}, {"keys", std::move(keys)}}});
  };
  const auto messages = converse_ready(
      socket(),
      {call("send_keys_batch", {{"operations", operation(repeated(512U), "Escape")}},
            1),
       call("send_keys_batch", {{"operations", operation(repeated(513U), "Escape")}},
            2),
       call("send_keys_batch", {{"operations", operation("%999", repeated(4096U))}}, 3),
       call("send_keys_batch", {{"operations", operation("%999", repeated(4097U))}},
            4)});

  for (const int id : {1, 3}) {
    const json* accepted = response(messages, id);
    ASSERT_NE(accepted, nullptr);
    EXPECT_TRUE(accepted->contains("result")) << accepted->dump();
  }
  for (const int id : {2, 4}) {
    const json* rejected = response(messages, id);
    ASSERT_NE(rejected, nullptr);
    ASSERT_TRUE(rejected->contains("error")) << rejected->dump();
    EXPECT_EQ((*rejected)["error"]["code"], -32602) << rejected->dump();
  }
}

TEST_F(McpProtocol, RejectsForceForPaneInputButKeepsRespawnForce) {
  const libtmux::Server server = connect_server();
  auto pane = server.pane("mcp");
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;
  libtmux::CaptureOptions capture_options;
  capture_options.whole_history = true;
  const auto buffers_before = server.buffers();
  ASSERT_TRUE(buffers_before.has_value()) << buffers_before.error().diagnostic;

  const auto messages = converse_ready(
      socket(),
      {call("send_keys", {{"paneId", pane->id()}, {"keys", "Z"}, {"force", true}}, 1),
       call("send_keys_batch",
            {{"operations", json::array({{{"paneId", pane->id()},
                                          {"keys", "batch-force-marker"},
                                          {"literal", true},
                                          {"enter", true},
                                          {"force", true}}})}},
            2),
       call("paste_text",
            {{"paneId", pane->id()}, {"text", "paste-force-marker"}, {"force", true}},
            3),
       call("run_shell_command",
            {{"paneId", pane->id()},
             {"command", "printf run-force-marker"},
             {"force", true}},
            4)});
  for (const int id : {1, 2, 3, 4}) {
    const json* rejected = response(messages, id);
    ASSERT_NE(rejected, nullptr);
    ASSERT_TRUE(rejected->contains("error")) << rejected->dump();
    EXPECT_EQ((*rejected)["error"]["code"], -32602) << rejected->dump();
  }

  const auto capture_after = pane->capture(capture_options);
  const auto buffers_after = server.buffers();
  ASSERT_TRUE(capture_after.has_value()) << capture_after.error().diagnostic;
  ASSERT_TRUE(buffers_after.has_value()) << buffers_after.error().diagnostic;
  for (const std::string_view marker :
       {"Z", "batch-force-marker", "paste-force-marker", "run-force-marker"}) {
    EXPECT_EQ(capture_after->find(marker), std::string::npos) << marker;
  }
  EXPECT_EQ(buffer_names(*buffers_after), buffer_names(*buffers_before));

  const json respawned =
      invoke("respawn_pane", {{"paneId", pane->id()}, {"force", true}}, 5);
  ASSERT_TRUE(respawned.contains("result")) << respawned.dump();
  EXPECT_FALSE(respawned["result"]["isError"].get<bool>()) << respawned.dump();
  EXPECT_EQ(respawned["result"]["structuredContent"]["pane_id"], pane->id());
}

TEST_F(McpProtocol, CreatesThenDiscoversAWindow) {
  const auto created_messages = converse_ready(
      socket(), {call("create_window", {{"session", "mcp"}, {"name", "from-mcp"}}, 1)});
  const json* created = response(created_messages, 1);
  ASSERT_NE(created, nullptr);
  ASSERT_FALSE((*created)["result"]["isError"].get<bool>());
  const std::string window_id =
      (*created)["result"]["structuredContent"]["window_id"].get<std::string>();
  EXPECT_EQ(window_id.front(), '@');

  const auto typed_messages = converse_ready(
      socket(), {call("paste_text", {{"paneId", window_id}, {"text", "marker"}}, 2)});
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
      socket(), {call("send_keys", {{"paneId", "mcp"}, {"keys", "Enter"}}, 1),
                 call("send_keys", {{"paneId", "mcp"}, {"keys", "NotAKey"}}, 2)});
  const json* sent = response(messages, 1);
  const json* invalid = response(messages, 2);
  ASSERT_NE(sent, nullptr);
  ASSERT_NE(invalid, nullptr);
  EXPECT_FALSE((*sent)["result"]["isError"].get<bool>());
  EXPECT_EQ((*invalid)["error"]["code"], -32602);
}

TEST_F(McpProtocol, RunShellCommandWaitsForItsOwnValidCompletionRecord) {
  const libtmux::Server server = connect_server();
  auto pane = server.pane("mcp");
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;
  ASSERT_TRUE(
      server.run({"resize-window", "-t", "mcp", "-x", "40", "-y", "24"}).has_value());
  ASSERT_TRUE(pane->send_text("printf '\\n__LIBTMUX_MCP_DONE_1__:7\\n'; "
                              "tmux wait-for -S mcp-legacy-marker-ready")
                  .has_value());
  ASSERT_TRUE(pane->send_key("Enter").has_value());
  ASSERT_TRUE(
      server.wait_for("mcp-legacy-marker-ready", std::chrono::seconds{2}).has_value());

  const json reply =
      invoke("run_shell_command",
             {{"paneId", pane->id()},
              {"command", "printf '\\n%s:0\\n' \"$__libtmux_mcp_marker\"; sleep 1; "
                          "sh -c 'exit 23'"},
              {"timeoutMs", 5000}},
             1, std::chrono::milliseconds{1500});
  ASSERT_FALSE(reply["result"]["isError"].get<bool>()) << reply.dump();
  EXPECT_EQ(reply["result"]["structuredContent"]["exit_code"], 23);

  const json shadowed =
      invoke("run_shell_command",
             {{"paneId", pane->id()},
              {"command", "printf() { command printf '\\n%s%s%s:%s\\n' \"$2\" \"$3\" "
                          "\"$4\" 0; }; sh -c 'exit 23'"},
              {"timeoutMs", 1000}},
             2, std::chrono::milliseconds{1500});
  ASSERT_FALSE(shadowed["result"]["isError"].get<bool>()) << shadowed.dump();
  EXPECT_EQ(shadowed["result"]["structuredContent"]["exit_code"], 23);

  ASSERT_TRUE(server.run({"set-option", "-g", "history-limit", "100"}).has_value());
  ASSERT_TRUE(server.run({"new-window", "-d", "-t", "mcp:", "-n", "mcp-short-history"})
                  .has_value());
  auto history_pane = server.pane("mcp:mcp-short-history");
  ASSERT_TRUE(history_pane.has_value()) << history_pane.error().diagnostic;
  const json evicted = invoke(
      "run_shell_command",
      {{"paneId", history_pane->id()},
       {"command", "i=0; while [ \"$i\" -lt 300 ]; do printf 'eviction-%s\\n' \"$i\"; "
                   "i=$((i+1)); done; sh -c 'exit 19'"},
       {"timeoutMs", 1000}},
      3, std::chrono::milliseconds{1500});
  ASSERT_FALSE(evicted["result"]["isError"].get<bool>()) << evicted.dump();
  EXPECT_EQ(evicted["result"]["structuredContent"]["exit_code"], 19);
  EXPECT_NE(evicted["result"]["structuredContent"]["text"].get<std::string>().find(
                "eviction-299"),
            std::string::npos);
}

TEST_F(McpProtocol, KeepsTimedOutRunInputReservedUntilCompletion) {
  const libtmux::Server server = connect_server();
  auto pane = server.pane("mcp");
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;
  const std::string pane_id{pane->id()};
  const std::string start =
      encode_requests({initialize_request(), initialized_notification(),
                       call("run_shell_command",
                            {{"paneId", pane_id},
                             {"command", "sleep 1; printf run-finished"},
                             {"timeoutMs", 50}},
                            1)});
  const std::string blocked = encode_requests(
      {call("paste_text", {{"paneId", pane_id}, {"text", "blocked-input"}}, 2)});
  const std::string after = encode_requests(
      {call("paste_text", {{"paneId", pane_id}, {"text", "after-run"}}, 3)});
  const auto messages =
      converse_steps(socket(), {{start, std::chrono::milliseconds{150}},
                                {blocked, std::chrono::milliseconds{1200}},
                                {after, std::chrono::milliseconds{300}}});

  const json* timed_out = response(messages, 1);
  const json* refused = response(messages, 2);
  const json* accepted = response(messages, 3);
  ASSERT_NE(timed_out, nullptr);
  ASSERT_NE(refused, nullptr);
  ASSERT_NE(accepted, nullptr);
  EXPECT_TRUE((*timed_out)["result"]["isError"].get<bool>());
  EXPECT_NE(
      (*timed_out)["result"]["content"][0]["text"].get<std::string>().find("timed out"),
      std::string::npos);
  EXPECT_TRUE((*refused)["result"]["isError"].get<bool>());
  EXPECT_NE((*refused)["result"]["content"][0]["text"].get<std::string>().find(
                "still active"),
            std::string::npos);
  EXPECT_FALSE((*accepted)["result"]["isError"].get<bool>());
  const std::string capture = captured(*pane);
  EXPECT_EQ(capture.find("blocked-input"), std::string::npos);
  EXPECT_NE(capture.find("run-finished"), std::string::npos);
  EXPECT_NE(capture.find("after-run"), std::string::npos);
}

TEST_F(McpProtocol, KeepsCancelledRunInputReservedUntilCompletion) {
  const libtmux::Server server = connect_server();
  auto pane = server.pane("mcp");
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;
  const auto tmux_executable = executable_on_path("tmux");
  ASSERT_TRUE(tmux_executable.has_value());
  const std::string pane_id{pane->id()};
  const std::string started = "mcp-cancelled-run-started";
  const std::string signal_started = shell_quote(tmux_executable->string()) +
                                     " -N -S " + shell_quote(socket().string()) +
                                     " wait-for -S " + shell_quote(started) + "; ";
  const std::string start = encode_requests(
      {initialize_request(), initialized_notification(),
       call("run_shell_command",
            {{"paneId", pane_id},
             {"command", signal_started + "sleep 1; printf cancelled-run-finished"},
             {"timeoutMs", 5000}},
            10)});
  const std::string cancel_and_block = encode_requests(
      {json{{"jsonrpc", "2.0"},
            {"method", "notifications/cancelled"},
            {"params", {{"requestId", 10}, {"reason", "test complete"}}}},
       call("send_keys", {{"paneId", pane_id}, {"keys", "Escape"}}, 11)});
  const std::string after = encode_requests(
      {call("paste_text", {{"paneId", pane_id}, {"text", "after-cancel"}}, 12)});
  const auto wait_until_started = [&]() -> libtmux::expected<void, std::string> {
    const auto ready = server.wait_for(started, std::chrono::seconds{2});
    if (!ready.has_value()) {
      return libtmux::unexpected(ready.error().diagnostic);
    }
    return {};
  };
  const auto messages = converse_steps(
      socket(),
      {{.text = start, .barrier_after_write = wait_until_started},
       {.text = cancel_and_block, .pause_after = std::chrono::milliseconds{1200}},
       {.text = after, .pause_after = std::chrono::milliseconds{300}}});

  EXPECT_EQ(response(messages, 10), nullptr);
  const json* refused = response(messages, 11);
  const json* accepted = response(messages, 12);
  ASSERT_NE(refused, nullptr);
  ASSERT_NE(accepted, nullptr);
  EXPECT_TRUE((*refused)["result"]["isError"].get<bool>());
  EXPECT_NE((*refused)["result"]["content"][0]["text"].get<std::string>().find(
                "still active"),
            std::string::npos);
  EXPECT_FALSE((*accepted)["result"]["isError"].get<bool>());
  const std::string capture = captured(*pane);
  EXPECT_NE(capture.find("cancelled-run-finished"), std::string::npos);
  EXPECT_NE(capture.find("after-cancel"), std::string::npos);
}

TEST_F(McpProtocol, ReleasesRetainedRunInputAfterPaneDeath) {
  const libtmux::Server server = connect_server();
  auto pane = server.pane("mcp");
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;
  const auto tmux_executable = executable_on_path("tmux");
  ASSERT_TRUE(tmux_executable.has_value());
  const std::string pane_id{pane->id()};
  ASSERT_TRUE(server.run({"set-option", "-w", "-t", pane_id, "remain-on-exit", "on"})
                  .has_value());

  const std::string started = "mcp-dead-run-started";
  const std::string release = "mcp-dead-run-release";
  const std::string exact_tmux = shell_quote(tmux_executable->string()) + " -N -S " +
                                 shell_quote(socket().string());
  const std::string command = exact_tmux + " wait-for -S " + shell_quote(started) +
                              "; " + exact_tmux + " wait-for " + shell_quote(release) +
                              "; kill -KILL $$";
  const std::string start = encode_requests(
      {initialize_request(), initialized_notification(),
       call("run_shell_command",
            {{"paneId", pane_id}, {"command", command}, {"timeoutMs", 50}}, 20)});
  const std::string respawn =
      encode_requests({call("respawn_pane", {{"paneId", pane_id}}, 21)});
  const std::string input = encode_requests(
      {call("send_keys", {{"paneId", pane_id}, {"keys", "Escape"}}, 22)});
  const auto finish_run = [&]() -> libtmux::expected<void, std::string> {
    const auto ready = server.wait_for(started, std::chrono::seconds{2});
    if (!ready.has_value()) {
      return libtmux::unexpected(ready.error().diagnostic);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{500});
    const auto released = server.signal(release);
    if (!released.has_value()) {
      return libtmux::unexpected(released.error().diagnostic);
    }
    const auto dead = wait_for_pane_value(*pane, "#{pane_dead}", "1");
    if (!dead.has_value()) {
      return libtmux::unexpected(dead.error().diagnostic);
    }
    if (*dead != "1") {
      return libtmux::unexpected(std::string{"pane did not become dead"});
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    return {};
  };
  const auto wait_until_respawned = [&]() -> libtmux::expected<void, std::string> {
    const auto alive = wait_for_pane_value(*pane, "#{pane_dead}", "0");
    if (!alive.has_value()) {
      return libtmux::unexpected(alive.error().diagnostic);
    }
    if (*alive != "0") {
      return libtmux::unexpected(std::string{"pane did not respawn"});
    }
    return {};
  };
  const auto messages = converse_steps(
      socket(), {{.text = start, .barrier_after_write = finish_run},
                 {.text = respawn, .barrier_after_write = wait_until_respawned},
                 {.text = input, .pause_after = std::chrono::milliseconds{300}}});

  const json* timed_out = response(messages, 20);
  const json* respawned = response(messages, 21);
  const json* accepted = response(messages, 22);
  ASSERT_NE(timed_out, nullptr);
  ASSERT_NE(respawned, nullptr);
  ASSERT_NE(accepted, nullptr);
  EXPECT_TRUE((*timed_out)["result"]["isError"].get<bool>());
  EXPECT_NE(
      (*timed_out)["result"]["content"][0]["text"].get<std::string>().find("timed out"),
      std::string::npos);
  EXPECT_FALSE((*respawned)["result"]["isError"].get<bool>());
  EXPECT_FALSE((*accepted)["result"]["isError"].get<bool>());
}

TEST_F(McpProtocol, IsolatesCompletionFramingAcrossInstalledPosixShells) {
  const auto tmux_executable = executable_on_path("tmux");
  ASSERT_TRUE(tmux_executable.has_value());
  const libtmux::Server server = connect_server();
  const std::array<std::string_view, 4> names{"sh", "bash", "dash", "zsh"};
  const std::array<std::tuple<std::string_view, std::string_view, bool, bool>, 6> flags{
      {
          {"neither", "\\set +e; \\set +x; \\set +f", false, false},
          {"errexit", "\\set +x; \\set +f; \\set -e", true, false},
          {"xtrace", "\\set +e; \\set +f; \\set -x", false, false},
          {"both", "\\set +e; \\set +x; \\set +f; \\set -e; \\set -x", true, false},
          {"noglob", "\\set +e; \\set +x; \\set -f", false, true},
          {"all", "\\set +e; \\set +x; \\set -e; \\set -x; \\set -f", true, true},
      }};
  int request_id = 100;

  for (const std::string_view name : names) {
    const auto shell = executable_on_path(name);
    if (!shell.has_value()) {
      std::cout << "Skipping unavailable shell: " << name << '\n';
      continue;
    }
    const auto created = server.run({"new-window", "-d", "-P", "-F", "#{pane_id}", "-t",
                                     "mcp:", "-n", "frame-" + std::string{name}});
    ASSERT_TRUE(created.has_value()) << created.error().diagnostic;
    const std::string pane_id = created->substr(0U, created->find('\n'));
    auto pane = server.pane(pane_id);
    ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;

    std::string shell_flags{" -i"};
    if (name == "bash") {
      shell_flags = " --noprofile --norc -i";
    } else if (name == "zsh") {
      shell_flags = " -f -i";
    }
    ASSERT_TRUE(pane->send_text("exec " + shell_quote(shell->string()) + shell_flags)
                    .has_value());
    ASSERT_TRUE(pane->send_key("Enter").has_value());
    const auto foreground = wait_for_pane_value(*pane, "#{pane_current_command}",
                                                shell->filename().string());
    ASSERT_TRUE(foreground.has_value()) << foreground.error().diagnostic;
    ASSERT_EQ(*foreground, shell->filename().string());

    const std::filesystem::path state_directory =
        socket().parent_path() / ("frame-state-" + std::string{name});
    std::filesystem::create_directories(state_directory);
    const std::filesystem::path exit_marker = state_directory / "parent-exit";
    const std::string exit_trap = ": > " + shell_quote(exit_marker.string());
    std::string traps = "\\trap " + shell_quote(exit_trap) + " 0";
    if (name == "bash") {
      traps = "\\trap " + shell_quote(exit_trap) + " EXIT; \\trap ':' ERR DEBUG";
    } else if (name == "zsh") {
      traps = "\\trap " + shell_quote(exit_trap) + " EXIT; \\trap ':' ZERR DEBUG";
    }
    send_shell_and_wait(
        server, *pane, *tmux_executable,
        "PS1=''; printf() { return 97; }; alias printf='false'; "
        "alias echo='false'; tmux() { return 98; }; "
        "readonly status rc __tmux_mcp_status; MCP_PARENT_VALUE=kept; "
        "export MCP_PARENT_EXPORT=kept; mcp_parent_function() { return 0; }; cd " +
            shell_quote(state_directory.string()) + "; " + traps +
            "; \\set +e; \\set +x");

    const std::string body = "body-" + std::string{name};
    const json output = invoke("run_shell_command",
                               {{"paneId", pane_id},
                                {"command", "command \\printf '" + body + "\\n'"},
                                {"timeoutMs", 2000}},
                               ++request_id);
    ASSERT_FALSE(output["result"]["isError"].get<bool>()) << output.dump();
    EXPECT_EQ(output["result"]["structuredContent"]["exit_code"], 0);
    EXPECT_EQ(output["result"]["structuredContent"]["text"], body + "\n");
    EXPECT_FALSE(std::filesystem::exists(exit_marker)) << name;

    const std::string trailing = "trailing-" + std::string{name};
    const json no_newline =
        invoke("run_shell_command",
               {{"paneId", pane_id},
                {"command", "command \\printf '" + trailing + "' # trailing comment"},
                {"timeoutMs", 2000}},
               ++request_id);
    ASSERT_FALSE(no_newline["result"]["isError"].get<bool>()) << no_newline.dump();
    EXPECT_EQ(no_newline["result"]["structuredContent"]["exit_code"], 0);
    EXPECT_EQ(no_newline["result"]["structuredContent"]["text"], trailing);

    const json syntax =
        invoke("run_shell_command",
               {{"paneId", pane_id}, {"command", "if then"}, {"timeoutMs", 2000}},
               ++request_id);
    ASSERT_FALSE(syntax["result"]["isError"].get<bool>()) << syntax.dump();
    EXPECT_NE(syntax["result"]["structuredContent"]["exit_code"], 0);

    const json bare_exit =
        invoke("run_shell_command",
               {{"paneId", pane_id}, {"command", "exit 23"}, {"timeoutMs", 2000}},
               ++request_id);
    ASSERT_FALSE(bare_exit["result"]["isError"].get<bool>()) << bare_exit.dump();
    EXPECT_EQ(bare_exit["result"]["structuredContent"]["exit_code"], 23);

    for (const auto& [flag_name, flag_command, inherited_errexit, inherited_noglob] :
         flags) {
      send_shell_and_wait(server, *pane, *tmux_executable, std::string{flag_command});
      const std::string parent_before =
          parent_shell_state(server, *pane, *tmux_executable,
                             std::string{name} + "-" + std::string{flag_name});
      const std::string checks =
          "[ \"$PWD\" = " + shell_quote(state_directory.string()) +
          " ] || exit 90; [ \"$MCP_PARENT_VALUE\" = kept ] || exit 91; "
          "[ \"$MCP_PARENT_EXPORT\" = kept ] || exit 92; "
          "mcp_parent_function || exit 93; "
          "sh -c '[ \"$MCP_PARENT_EXPORT\" = kept ]' || exit 94; " +
          (inherited_noglob ? "case $- in *f*) ;; *) exit 95 ;; esac"
                            : "case $- in *f*) exit 95 ;; *) : ;; esac");
      const json isolated = invoke(
          "run_shell_command",
          {{"paneId", pane_id},
           {"command", checks + "; mcp_child_function() { return 0; }; \\trap ':' 0; "
                                "cd /; MCP_PARENT_VALUE=changed; "
                                "MCP_PARENT_EXPORT=changed; export MCP_PARENT_EXPORT; "
                                "exit 23"},
           {"timeoutMs", 2000}},
          ++request_id);
      ASSERT_FALSE(isolated["result"]["isError"].get<bool>()) << isolated.dump();
      EXPECT_EQ(isolated["result"]["structuredContent"]["exit_code"], 23)
          << name << ' ' << flag_name;
      EXPECT_FALSE(contains_completion_marker(
          isolated["result"]["structuredContent"]["text"].get<std::string>()));

      const std::filesystem::path side_effect =
          state_directory / ("after-false-" + std::string{flag_name});
      std::error_code ignored;
      std::filesystem::remove(side_effect, ignored);
      const json errexit =
          invoke("run_shell_command",
                 {{"paneId", pane_id},
                  {"command", "false; : > " + shell_quote(side_effect.string())},
                  {"timeoutMs", 2000}},
                 ++request_id);
      ASSERT_FALSE(errexit["result"]["isError"].get<bool>()) << errexit.dump();
      EXPECT_EQ(errexit["result"]["structuredContent"]["exit_code"],
                inherited_errexit ? 1 : 0)
          << name << ' ' << flag_name;
      EXPECT_EQ(std::filesystem::exists(side_effect), !inherited_errexit)
          << name << ' ' << flag_name;
      EXPECT_FALSE(contains_completion_marker(
          errexit["result"]["structuredContent"]["text"].get<std::string>()));

      const json parent_check =
          invoke("run_shell_command",
                 {{"paneId", pane_id}, {"command", checks}, {"timeoutMs", 2000}},
                 ++request_id);
      ASSERT_FALSE(parent_check["result"]["isError"].get<bool>())
          << parent_check.dump();
      EXPECT_EQ(parent_check["result"]["structuredContent"]["exit_code"], 0)
          << name << ' ' << flag_name;
      const std::string parent_after =
          parent_shell_state(server, *pane, *tmux_executable,
                             std::string{name} + "-" + std::string{flag_name});
      EXPECT_EQ(parent_after, parent_before) << name << ' ' << flag_name;
    }

    ASSERT_TRUE(pane->set_option("remain-on-exit", "on").has_value());
    ASSERT_TRUE(pane->send_text("exit").has_value());
    ASSERT_TRUE(pane->send_key("Enter").has_value());
    const auto dead = wait_for_pane_value(*pane, "#{pane_dead}", "1");
    ASSERT_TRUE(dead.has_value()) << dead.error().diagnostic;
    ASSERT_EQ(*dead, "1");
    EXPECT_TRUE(std::filesystem::exists(exit_marker)) << name;
  }
}

TEST_F(McpProtocol, PreservesInheritedErrorAndDebugTrapsAcrossBashAndZsh) {
  const auto tmux_executable = executable_on_path("tmux");
  ASSERT_TRUE(tmux_executable.has_value());
  const libtmux::Server server = connect_server();
  int request_id = 200;

  for (const std::string_view name :
       {std::string_view{"bash"}, std::string_view{"zsh"}}) {
    const auto shell = executable_on_path(name);
    if (!shell.has_value()) {
      std::cout << "Skipping unavailable shell: " << name << '\n';
      continue;
    }
    const auto created = server.run({"new-window", "-d", "-P", "-F", "#{pane_id}", "-t",
                                     "mcp:", "-n", "traps-" + std::string{name}});
    ASSERT_TRUE(created.has_value()) << created.error().diagnostic;
    const std::string pane_id = created->substr(0U, created->find('\n'));
    auto pane = server.pane(pane_id);
    ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;

    const std::string flags = name == "bash" ? " --noprofile --norc -i" : " -f -i";
    ASSERT_TRUE(
        pane->send_text("exec " + shell_quote(shell->string()) + flags).has_value());
    ASSERT_TRUE(pane->send_key("Enter").has_value());
    const auto foreground = wait_for_pane_value(*pane, "#{pane_current_command}",
                                                shell->filename().string());
    ASSERT_TRUE(foreground.has_value()) << foreground.error().diagnostic;
    ASSERT_EQ(*foreground, shell->filename().string());

    const std::string debug_out = "trap-debug-" + std::string{name} + "-stdout";
    const std::string debug_error = "trap-debug-" + std::string{name} + "-stderr";
    const std::string error_out = "trap-error-" + std::string{name} + "-stdout";
    const std::string error_error = "trap-error-" + std::string{name} + "-stderr";
    const std::string debug_action =
        "/usr/bin/printf '%s\\n' " + shell_quote(debug_out) +
        "; /usr/bin/printf '%s\\n' " + shell_quote(debug_error) + " >&2";
    const std::string error_action =
        "/usr/bin/printf '%s\\n' " + shell_quote(error_out) +
        "; /usr/bin/printf '%s\\n' " + shell_quote(error_error) + " >&2";
    const std::string error_signal = name == "bash" ? "ERR" : "ZERR";
    send_shell_and_wait(server, *pane, *tmux_executable,
                        "PS1=''; \\trap " + shell_quote(debug_action) +
                            " DEBUG; \\trap " + shell_quote(error_action) + " " +
                            error_signal + "; \\set -e; \\set -x");

    const std::string parent_before = parent_shell_state(
        server, *pane, *tmux_executable, "traps-before-" + std::string{name});
    const auto files_before = run_shell_trap_files();
    const std::string success_marker = "trap-success-" + std::string{name};
    const json success = invoke("run_shell_command",
                                {{"paneId", pane_id},
                                 {"command", "case $- in *e*) ;; *) exit 90 ;; esac; "
                                             "case $- in *x*) ;; *) exit 91 ;; esac; "
                                             "/usr/bin/printf '%s\\n' " +
                                                 shell_quote(success_marker)},
                                 {"timeoutMs", 5000}},
                                ++request_id);
    ASSERT_FALSE(success["result"]["isError"].get<bool>()) << success.dump();
    EXPECT_EQ(success["result"]["structuredContent"]["exit_code"], 0) << name;
    const std::string success_text =
        success["result"]["structuredContent"]["text"].get<std::string>();
    EXPECT_EQ(line_occurrences(success_text, success_marker), 1U) << name;
    EXPECT_GT(line_occurrences(success_text, debug_out), 0U) << name;
    EXPECT_GT(line_occurrences(success_text, debug_error), 0U) << name;

    const std::string unreachable = "trap-unreachable-" + std::string{name};
    const json failure = invoke(
        "run_shell_command",
        {{"paneId", pane_id},
         {"command", "false; /usr/bin/printf '%s\\n' " + shell_quote(unreachable)},
         {"timeoutMs", 5000}},
        ++request_id);
    ASSERT_FALSE(failure["result"]["isError"].get<bool>()) << failure.dump();
    EXPECT_NE(failure["result"]["structuredContent"]["exit_code"], 0) << name;
    const std::string failure_text =
        failure["result"]["structuredContent"]["text"].get<std::string>();
    EXPECT_EQ(line_occurrences(failure_text, error_out), 1U) << name;
    EXPECT_EQ(line_occurrences(failure_text, error_error), 1U) << name;
    EXPECT_EQ(line_occurrences(failure_text, unreachable), 0U) << name;

    send_shell_and_wait(
        server, *pane, *tmux_executable,
        "\\set +x; __libtmux_test_trap=\"true; : "
        "$(/usr/bin/printf '%070000d' 0)\"; \\trap \"$__libtmux_test_trap\" " +
            error_signal + "; \\unset __libtmux_test_trap; \\set -x");
    const std::string refused_marker = "trap-refused-" + std::string{name};
    const json refused =
        invoke("run_shell_command",
               {{"paneId", pane_id},
                {"command", "/usr/bin/printf '%s\\n' " + shell_quote(refused_marker)},
                {"timeoutMs", 5000}},
               ++request_id);
    ASSERT_FALSE(refused["result"]["isError"].get<bool>()) << refused.dump();
    EXPECT_EQ(refused["result"]["structuredContent"]["exit_code"], 125) << name;
    const std::string refused_text =
        refused["result"]["structuredContent"]["text"].get<std::string>();
    EXPECT_EQ(line_occurrences(refused_text, refused_marker), 0U) << name;

    send_shell_and_wait(server, *pane, *tmux_executable,
                        "\\set +x; \\trap " + shell_quote(error_action) + " " +
                            error_signal + "; \\set -x");
    const std::string recovered_marker = "trap-recovered-" + std::string{name};
    const json recovered =
        invoke("run_shell_command",
               {{"paneId", pane_id},
                {"command", "/usr/bin/printf '%s\\n' " + shell_quote(recovered_marker)},
                {"timeoutMs", 5000}},
               ++request_id);
    ASSERT_FALSE(recovered["result"]["isError"].get<bool>()) << recovered.dump();
    EXPECT_EQ(recovered["result"]["structuredContent"]["exit_code"], 0) << name;
    EXPECT_EQ(line_occurrences(
                  recovered["result"]["structuredContent"]["text"].get<std::string>(),
                  recovered_marker),
              1U)
        << name;

    const std::string parent_after = parent_shell_state(
        server, *pane, *tmux_executable, "traps-after-" + std::string{name});
    EXPECT_EQ(parent_after, parent_before) << name;
    EXPECT_EQ(run_shell_trap_files(), files_before) << name;
  }
}

TEST_F(McpProtocol, RefusesEveryInputToolWhileTheNamedPaneIsModal) {
  const libtmux::Server server = connect_server();
  auto pane = server.pane("mcp");
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;
  ASSERT_TRUE(pane->enter_copy_mode().has_value());
  const auto buffers_before = server.buffers();
  ASSERT_TRUE(buffers_before.has_value()) << buffers_before.error().diagnostic;
  const auto messages = converse_ready(
      socket(),
      {call("send_keys", {{"paneId", pane->id()}, {"keys", "Escape"}}, 1),
       call("send_keys_batch",
            {{"operations", json::array({{{"paneId", pane->id()},
                                          {"keys", "modal-batch"},
                                          {"literal", true},
                                          {"enter", true}}})}},
            2),
       call("paste_text", {{"paneId", pane->id()}, {"text", "modal-paste"}}, 3),
       call("run_shell_command",
            {{"paneId", pane->id()}, {"command", "printf modal-run"}, {"timeoutMs", 1}},
            4)});

  std::vector<std::string> errors;
  for (const int id : {1, 3, 4}) {
    const json* reply = response(messages, id);
    ASSERT_NE(reply, nullptr);
    EXPECT_TRUE((*reply)["result"]["isError"].get<bool>());
    errors.push_back((*reply)["result"]["content"][0]["text"].get<std::string>());
  }
  const json* batch = response(messages, 2);
  ASSERT_NE(batch, nullptr);
  const json& batch_result = (*batch)["result"]["structuredContent"];
  EXPECT_EQ(batch_result["completed"], 0);
  ASSERT_EQ(batch_result["failures"].size(), 1U);
  errors.push_back(batch_result["failures"][0]["reason"].get<std::string>());
  for (const std::string& error : errors) {
    EXPECT_NE(error.find(pane->id()), std::string::npos);
    EXPECT_NE(error.find("human-owned"), std::string::npos);
  }
  const auto mode = pane->expand("#{pane_in_mode}");
  const std::string after = captured(*pane);
  const auto buffers_after = server.buffers();
  ASSERT_TRUE(mode.has_value()) << mode.error().diagnostic;
  ASSERT_TRUE(buffers_after.has_value()) << buffers_after.error().diagnostic;
  EXPECT_EQ(*mode, "1");
  for (const std::string_view marker : {"modal-batch", "modal-paste", "modal-run"}) {
    EXPECT_EQ(after.find(marker), std::string::npos) << marker;
  }
  EXPECT_EQ(buffer_names(*buffers_after), buffer_names(*buffers_before));
}

TEST_F(McpProtocol, KeepsEmptyPasteBufferFreeAndEnterTargetOnly) {
  const libtmux::Server server = connect_server();
  const auto panes = split_sorted_panes(server);
  ASSERT_EQ(panes.size(), 2U);
  const libtmux::Pane source = panes.front();
  const libtmux::Pane sibling = panes.back();
  ASSERT_TRUE(source.set_option("synchronize-panes", "on").has_value());
  ASSERT_TRUE(sibling.set_option("synchronize-panes", "on").has_value());
  const auto buffers_before = server.buffers();
  ASSERT_TRUE(buffers_before.has_value()) << buffers_before.error().diagnostic;

  const json missing =
      invoke("paste_text", {{"paneId", source.id()}, {"enter", false}}, 0);
  ASSERT_TRUE(missing.contains("error")) << missing.dump();
  EXPECT_EQ(missing["error"]["code"], -32602);

  ASSERT_TRUE(server
                  .run({"set-hook", "-g", "after-set-buffer",
                        "copy-mode -t " + std::string{source.id()}})
                  .has_value());
  const json empty = invoke(
      "paste_text", {{"paneId", source.id()}, {"text", ""}, {"enter", false}}, 1);
  ASSERT_TRUE(empty.contains("result")) << empty.dump();
  ASSERT_FALSE(empty["result"]["isError"].get<bool>()) << empty.dump();
  EXPECT_FALSE(empty["result"]["structuredContent"]["changed"].get<bool>());
  const auto mode = source.expand("#{pane_in_mode}");
  const auto buffers_after_empty = server.buffers();
  ASSERT_TRUE(mode.has_value()) << mode.error().diagnostic;
  ASSERT_TRUE(buffers_after_empty.has_value())
      << buffers_after_empty.error().diagnostic;
  EXPECT_EQ(*mode, "0");
  EXPECT_EQ(buffer_names(*buffers_after_empty), buffer_names(*buffers_before));

  const json refused =
      invoke("paste_text", {{"paneId", source.id()}, {"text", "late-refusal"}}, 2);
  ASSERT_TRUE(refused.contains("result")) << refused.dump();
  ASSERT_TRUE(refused["result"]["isError"].get<bool>()) << refused.dump();
  EXPECT_NE(
      refused["result"]["content"][0]["text"].get<std::string>().find("human-owned"),
      std::string::npos);
  const auto refused_mode = source.expand("#{pane_in_mode}");
  const auto buffers_after_refusal = server.buffers();
  ASSERT_TRUE(refused_mode.has_value()) << refused_mode.error().diagnostic;
  ASSERT_TRUE(buffers_after_refusal.has_value())
      << buffers_after_refusal.error().diagnostic;
  EXPECT_EQ(*refused_mode, "1");
  EXPECT_EQ(buffer_names(*buffers_after_refusal), buffer_names(*buffers_before));

  ASSERT_TRUE(server.run({"set-hook", "-gu", "after-set-buffer"}).has_value());
  ASSERT_TRUE(source.send_key("q").has_value());
  const auto cleared_mode = wait_for_pane_value(source, "#{pane_in_mode}", "0");
  ASSERT_TRUE(cleared_mode.has_value()) << cleared_mode.error().diagnostic;
  ASSERT_EQ(*cleared_mode, "0");
  const std::string marker = "target-only-paste-enter-marker";
  const std::string channel = "target-only-paste-enter-ready";
  const std::string sibling_before = captured(sibling);
  const json entered =
      invoke("paste_text",
             {{"paneId", source.id()},
              {"text", "printf " + marker + "; tmux wait-for -S " + channel},
              {"enter", true}},
             3);
  ASSERT_TRUE(entered.contains("result")) << entered.dump();
  ASSERT_FALSE(entered["result"]["isError"].get<bool>()) << entered.dump();
  EXPECT_TRUE(entered["result"]["structuredContent"]["changed"].get<bool>());
  const auto completed = server.wait_for(channel, std::chrono::seconds{2});
  ASSERT_TRUE(completed.has_value()) << completed.error().diagnostic;
  EXPECT_NE(captured(source).find(marker), std::string::npos);
  EXPECT_EQ(captured(sibling), sibling_before);
  const auto buffers_after_enter = server.buffers();
  ASSERT_TRUE(buffers_after_enter.has_value())
      << buffers_after_enter.error().diagnostic;
  EXPECT_EQ(buffer_names(*buffers_after_enter), buffer_names(*buffers_before));
}

TEST_F(McpProtocol, ResolvesAnAuthenticatedCallerOutsideTheTargetWindow) {
  const libtmux::Server server = connect_server();
  auto source = server.pane("mcp");
  ASSERT_TRUE(source.has_value()) << source.error().diagnostic;
  auto sessions = server.sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  ASSERT_EQ(sessions->size(), 1U);
  auto caller_window = sessions->front().new_window("mcp-caller");
  ASSERT_TRUE(caller_window.has_value()) << caller_window.error().diagnostic;
  auto caller_panes = caller_window->panes();
  ASSERT_TRUE(caller_panes.has_value()) << caller_panes.error().diagnostic;
  ASSERT_EQ(caller_panes->size(), 1U);
  const libtmux::Pane caller = caller_panes->front();
  const auto identity =
      server.run({"display-message", "-p", "-t", caller.id(), "#{pid} #{session_id}"});
  ASSERT_TRUE(identity.has_value()) << identity.error().diagnostic;
  std::string caller_identity = *identity;
  ASSERT_FALSE(caller_identity.empty());
  ASSERT_EQ(caller_identity.back(), '\n');
  caller_identity.pop_back();
  const std::size_t separator = caller_identity.find(' ');
  ASSERT_NE(separator, std::string::npos);
  ASSERT_LT(separator + 2U, caller_identity.size());
  ASSERT_EQ(caller_identity[separator + 1U], '$');

  auto environment = libtmux::test::current_environment();
  libtmux::test::set_environment(environment, "TMUX",
                                 socket().string() + "," +
                                     caller_identity.substr(0U, separator) + "," +
                                     caller_identity.substr(separator + 2U));
  libtmux::test::set_environment(environment, "TMUX_PANE", std::string{caller.id()});
  const auto messages = converse_with(
      {"--socket-path", socket().string()}, std::move(environment),
      {initialize_request(), initialized_notification(),
       call("paste_text", {{"paneId", source->id()}, {"text", ""}, {"enter", false}},
            1)});

  const json* reply = response(messages, 1);
  ASSERT_NE(reply, nullptr);
  ASSERT_TRUE(reply->contains("result")) << reply->dump();
  EXPECT_FALSE((*reply)["result"]["isError"].get<bool>()) << reply->dump();
}

TEST_F(McpProtocol, GuardsTheEffectiveSynchronizedCohortButPastesOnlyTheTarget) {
  const libtmux::Server server = connect_server();
  const auto panes = split_sorted_panes(server);
  ASSERT_EQ(panes.size(), 2U);
  const libtmux::Pane source = panes.front();
  const libtmux::Pane sibling = panes.back();
  auto window = source.window();
  ASSERT_TRUE(window.has_value()) << window.error().diagnostic;
  const auto window_default = window->option("synchronize-panes");
  ASSERT_TRUE(window_default.has_value()) << window_default.error().diagnostic;
  ASSERT_EQ(window_default->value, "off");
  ASSERT_TRUE(source.set_option("synchronize-panes", "on").has_value());
  ASSERT_TRUE(sibling.set_option("synchronize-panes", "on").has_value());
  ASSERT_TRUE(sibling.enter_copy_mode().has_value());
  ASSERT_TRUE(server.run({"choose-tree", "-t", sibling.id()}).has_value());
  const auto nested_mode = sibling.expand("#{pane_in_mode}");
  ASSERT_TRUE(nested_mode.has_value()) << nested_mode.error().diagnostic;
  ASSERT_EQ(*nested_mode, "2");

  const auto guarded = converse_ready(
      socket(), {call("send_keys", {{"paneId", source.id()}, {"keys", "Escape"}}, 1),
                 call("send_keys_batch",
                      {{"operations", json::array({{{"paneId", source.id()},
                                                    {"keys", "cohort-batch"},
                                                    {"literal", true},
                                                    {"enter", true}}})}},
                      2),
                 call("run_shell_command",
                      {{"paneId", source.id()},
                       {"command", "printf cohort-run"},
                       {"timeoutMs", 1000}},
                      3)});
  for (const int id : {1, 3}) {
    const json* reply = response(guarded, id);
    ASSERT_NE(reply, nullptr);
    EXPECT_TRUE((*reply)["result"]["isError"].get<bool>());
  }
  const json* batch = response(guarded, 2);
  ASSERT_NE(batch, nullptr);
  EXPECT_EQ((*batch)["result"]["structuredContent"]["completed"], 0);
  EXPECT_EQ((*batch)["result"]["structuredContent"]["failures"][0]["index"], 0);
  const std::string source_after = captured(source);
  const std::string sibling_after = captured(sibling);
  for (const std::string_view marker : {"cohort-batch", "cohort-run"}) {
    EXPECT_EQ(source_after.find(marker), std::string::npos) << marker;
    EXPECT_EQ(sibling_after.find(marker), std::string::npos) << marker;
  }

  const std::string paste_marker = "target-only-paste-marker";
  const std::string sibling_before_paste = captured(sibling);
  const json pasted =
      invoke("paste_text", {{"paneId", source.id()}, {"text", paste_marker}}, 10);
  ASSERT_FALSE(pasted["result"]["isError"].get<bool>()) << pasted.dump();
  const std::string source_after_paste = captured(source);
  const std::string sibling_after_paste = captured(sibling);
  const auto sibling_mode = sibling.expand("#{pane_in_mode}");
  ASSERT_TRUE(sibling_mode.has_value()) << sibling_mode.error().diagnostic;
  EXPECT_NE(source_after_paste.find(paste_marker), std::string::npos);
  EXPECT_EQ(sibling_after_paste.find(paste_marker), std::string::npos);
  EXPECT_EQ(sibling_after_paste, sibling_before_paste);
  EXPECT_EQ(*sibling_mode, "2");

  ASSERT_TRUE(sibling.set_option("synchronize-panes", "off").has_value());
  const json sent =
      invoke("send_keys", {{"paneId", source.id()}, {"keys", "Escape"}}, 11);
  ASSERT_FALSE(sent["result"]["isError"].get<bool>()) << sent.dump();
  EXPECT_EQ(sent["result"]["structuredContent"]["target_pane_ids"],
            json::array({source.id()}));

  ASSERT_TRUE(source.set_option("synchronize-panes", "off").has_value());
  ASSERT_TRUE(sibling.set_option("synchronize-panes", "on").has_value());
  const std::string source_only_marker = "source-off-delivery-marker";
  const json source_only =
      invoke("send_keys_batch",
             {{"operations", json::array({{{"paneId", source.id()},
                                           {"keys", source_only_marker},
                                           {"literal", true},
                                           {"enter", true}}})}},
             12);
  ASSERT_FALSE(source_only["result"]["isError"].get<bool>()) << source_only.dump();
  EXPECT_EQ(source_only["result"]["structuredContent"]["targets"][0]["resolvedPaneIds"],
            json::array({source.id()}));
  const std::string source_only_capture = captured(source);
  const std::string sibling_source_off = captured(sibling);
  EXPECT_NE(source_only_capture.find(source_only_marker), std::string::npos);
  EXPECT_EQ(sibling_source_off.find(source_only_marker), std::string::npos);

  ASSERT_TRUE(sibling.set_option("synchronize-panes", "off").has_value());
  ASSERT_TRUE(server.run({"respawn-pane", "-k", "-t", sibling.id()}).has_value());
  const auto cleared_mode = wait_for_pane_value(sibling, "#{pane_in_mode}", "0");
  ASSERT_TRUE(cleared_mode.has_value()) << cleared_mode.error().diagnostic;
  ASSERT_EQ(*cleared_mode, "0");
  ASSERT_TRUE(sibling.set_option("synchronize-panes", "on").has_value());
  ASSERT_TRUE(source.set_option("synchronize-panes", "on").has_value());
  const json refused =
      invoke("run_shell_command",
             {{"paneId", source.id()}, {"command", "printf synchronized-run"}}, 13);
  ASSERT_TRUE(refused["result"]["isError"].get<bool>()) << refused.dump();
  const std::string error = refused["result"]["content"][0]["text"];
  EXPECT_NE(error.find(source.id()), std::string::npos);
  EXPECT_NE(error.find(sibling.id()), std::string::npos);
  EXPECT_NE(error.find("disable synchronize-panes"), std::string::npos);
  EXPECT_EQ(error.find("human-owned"), std::string::npos);
  EXPECT_EQ(error.find("mode"), std::string::npos);
  EXPECT_EQ(captured(source).find("synchronized-run"), std::string::npos);
  EXPECT_EQ(captured(sibling).find("synchronized-run"), std::string::npos);

  ASSERT_TRUE(source.set_option("synchronize-panes", "off").has_value());
  ASSERT_TRUE(sibling.set_option("remain-on-exit", "on").has_value());
  ASSERT_TRUE(sibling.send_text("exit").has_value());
  ASSERT_TRUE(sibling.send_key("Enter").has_value());
  const auto sibling_dead = wait_for_pane_value(sibling, "#{pane_dead}", "1");
  ASSERT_TRUE(sibling_dead.has_value()) << sibling_dead.error().diagnostic;
  ASSERT_EQ(*sibling_dead, "1");
  ASSERT_TRUE(source.set_option("synchronize-panes", "on").has_value());
  const std::string dead_cohort_marker = "dead-cohort-marker";
  const auto dead_guarded = converse_ready(
      socket(), {call("send_keys", {{"paneId", source.id()}, {"keys", "Escape"}}, 20),
                 call("send_keys_batch",
                      {{"operations", json::array({{{"paneId", source.id()},
                                                    {"keys", dead_cohort_marker},
                                                    {"literal", true}}})}},
                      21)});
  const json* dead_send = response(dead_guarded, 20);
  const json* dead_batch = response(dead_guarded, 21);
  ASSERT_NE(dead_send, nullptr);
  ASSERT_NE(dead_batch, nullptr);
  EXPECT_TRUE((*dead_send)["result"]["isError"].get<bool>());
  EXPECT_EQ((*dead_batch)["result"]["structuredContent"]["completed"], 0);
  EXPECT_NE(
      (*dead_send)["result"]["content"][0]["text"].get<std::string>().find("dead"),
      std::string::npos);
  EXPECT_EQ(captured(source).find(dead_cohort_marker), std::string::npos);

  ASSERT_TRUE(source.set_option("synchronize-panes", "off").has_value());
  const json singular =
      invoke("run_shell_command",
             {{"paneId", source.id()}, {"command", "printf source-only-run"}}, 14);
  ASSERT_FALSE(singular["result"]["isError"].get<bool>()) << singular.dump();
  EXPECT_EQ(singular["result"]["structuredContent"]["exit_code"], 0);
  EXPECT_EQ(captured(sibling).find("source-only-run"), std::string::npos);
}

TEST_F(McpProtocol, RefusesEveryInputToolForADeadConfiguredPane) {
  const libtmux::Server server = connect_server();
  auto pane = server.pane("mcp");
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;
  ASSERT_TRUE(pane->set_option("remain-on-exit", "on").has_value());
  ASSERT_TRUE(pane->send_text("exit").has_value());
  ASSERT_TRUE(pane->send_key("Enter").has_value());
  const auto dead = wait_for_pane_value(*pane, "#{pane_dead}", "1");
  ASSERT_TRUE(dead.has_value()) << dead.error().diagnostic;
  ASSERT_EQ(*dead, "1");
  const auto buffers_before = server.buffers();
  ASSERT_TRUE(buffers_before.has_value()) << buffers_before.error().diagnostic;
  const auto replies = converse_ready(
      socket(),
      {call("send_keys", {{"paneId", pane->id()}, {"keys", "Escape"}}, 1),
       call("send_keys_batch",
            {{"operations", json::array({{{"paneId", pane->id()},
                                          {"keys", "dead-batch"},
                                          {"literal", true},
                                          {"enter", true}}})}},
            2),
       call("paste_text", {{"paneId", pane->id()}, {"text", "dead-paste"}}, 3),
       call("run_shell_command",
            {{"paneId", pane->id()}, {"command", "printf dead-run"}, {"timeoutMs", 1}},
            4)});
  for (const int id : {1, 3, 4}) {
    const json* reply = response(replies, id);
    ASSERT_NE(reply, nullptr);
    EXPECT_TRUE((*reply)["result"]["isError"].get<bool>());
    EXPECT_NE((*reply)["result"]["content"][0]["text"].get<std::string>().find("dead"),
              std::string::npos);
  }
  const json* batch = response(replies, 2);
  ASSERT_NE(batch, nullptr);
  EXPECT_EQ((*batch)["result"]["structuredContent"]["completed"], 0);
  EXPECT_NE((*batch)["result"]["structuredContent"]["failures"][0]["reason"]
                .get<std::string>()
                .find("dead"),
            std::string::npos);
  const auto buffers_after = server.buffers();
  ASSERT_TRUE(buffers_after.has_value()) << buffers_after.error().diagnostic;
  EXPECT_EQ(buffer_names(*buffers_after), buffer_names(*buffers_before));
  const std::string after = captured(*pane);
  for (const std::string_view marker : {"dead-batch", "dead-paste", "dead-run"}) {
    EXPECT_EQ(after.find(marker), std::string::npos) << marker;
  }
}

TEST_F(McpProtocol, RefusesShellCommandsForANonShellForegroundProcess) {
  const libtmux::Server server = connect_server();
  auto pane = server.pane("mcp");
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;
  const std::string pane_id{pane->id()};
  ASSERT_TRUE(pane->send_text("exec cat").has_value());
  ASSERT_TRUE(pane->send_key("Enter").has_value());
  const auto command = wait_for_pane_value(*pane, "#{pane_current_command}", "cat");
  ASSERT_TRUE(command.has_value()) << command.error().diagnostic;
  ASSERT_EQ(*command, "cat");

  const std::string marker = "non-shell-payload-marker";
  const json reply =
      invoke("run_shell_command",
             {{"paneId", pane_id}, {"command", "printf " + marker}, {"timeoutMs", 100}},
             1, std::chrono::milliseconds{250});
  ASSERT_TRUE(reply["result"]["isError"].get<bool>()) << reply.dump();
  EXPECT_NE(reply["result"]["content"][0]["text"].get<std::string>().find(
                "supported POSIX shell"),
            std::string::npos);
  EXPECT_EQ(captured(*pane).find(marker), std::string::npos);
}

TEST_F(McpProtocol, BatchPreflightsEachRowBeforeTextAndOptionalEnter) {
  const libtmux::Server server = connect_server();
  const auto panes = split_sorted_panes(server);
  ASSERT_EQ(panes.size(), 2U);
  const libtmux::Pane modal = panes.front();
  const libtmux::Pane clear = panes.back();
  ASSERT_TRUE(modal.enter_copy_mode().has_value());
  const std::string refused_marker = "batch-refused-marker";
  const std::string delivered_marker = "batch-delivered-marker";
  const json operations = json::array(
      {{{"paneId", modal.id()},
        {"keys", refused_marker},
        {"literal", true},
        {"enter", true}},
       {{"paneId", clear.id()}, {"keys", delivered_marker}, {"literal", true}}});

  const json reply = invoke("send_keys_batch",
                            {{"onError", "continue"}, {"operations", operations}}, 1);
  ASSERT_FALSE(reply["result"]["isError"].get<bool>()) << reply.dump();
  const json& result = reply["result"]["structuredContent"];
  EXPECT_EQ(result["completed"], 1);
  ASSERT_EQ(result["failures"].size(), 1U);
  EXPECT_EQ(result["failures"][0]["index"], 0);
  EXPECT_NE(result["failures"][0]["reason"].get<std::string>().find("human-owned"),
            std::string::npos);
  ASSERT_EQ(result["targets"].size(), 1U);
  EXPECT_EQ(result["targets"][0]["index"], 1);
  EXPECT_EQ(result["targets"][0]["resolvedPaneIds"], json::array({clear.id()}));

  const std::string modal_after = captured(modal);
  const std::string clear_after = captured(clear);
  const auto modal_mode = modal.expand("#{pane_in_mode}");
  ASSERT_TRUE(modal_mode.has_value()) << modal_mode.error().diagnostic;
  EXPECT_EQ(modal_after.find(refused_marker), std::string::npos);
  EXPECT_NE(clear_after.find(delivered_marker), std::string::npos);
  EXPECT_EQ(*modal_mode, "1");
}

TEST_F(McpProtocol, ReportsTargetsExpandedBySynchronizedPaneInput) {
  const auto split_messages =
      converse_ready(socket(), {call("split_window", {{"paneId", "mcp"}}, 1)});
  const json* split = response(split_messages, 1);
  ASSERT_NE(split, nullptr);
  ASSERT_FALSE((*split)["result"]["isError"].get<bool>());

  const auto listed_messages =
      converse_ready(socket(), {call("list_panes", json::object(), 2)});
  const json* listed = response(listed_messages, 2);
  ASSERT_NE(listed, nullptr);
  const json& panes = (*listed)["result"]["structuredContent"]["panes"];
  ASSERT_EQ(panes.size(), 2U);
  const std::string pane_id = panes[0]["id"].get<std::string>();
  const std::string window_id = panes[0]["window_id"].get<std::string>();
  std::vector<std::string> expected_ids;
  for (const json& pane : panes) {
    expected_ids.push_back(pane["id"].get<std::string>());
  }
  std::ranges::sort(expected_ids);
  const json expected = expected_ids;

  const auto synchronized_messages = converse_ready(
      socket(),
      {call("set_synchronize_panes", {{"windowId", window_id}, {"enabled", true}}, 3)});
  const json* synchronized = response(synchronized_messages, 3);
  ASSERT_NE(synchronized, nullptr);
  ASSERT_TRUE(synchronized->contains("result")) << synchronized->dump();
  ASSERT_FALSE((*synchronized)["result"]["isError"].get<bool>());

  const auto sent_messages = converse_ready(
      socket(), {call("send_keys", {{"paneId", pane_id}, {"keys", "Escape"}}, 4)});
  const auto batched_messages = converse_ready(
      socket(),
      {call("send_keys_batch",
            {{"operations", json::array({{{"paneId", pane_id}, {"keys", "Escape"}}})}},
            5)});
  const json* sent = response(sent_messages, 4);
  const json* batched = response(batched_messages, 5);
  ASSERT_NE(sent, nullptr);
  ASSERT_NE(batched, nullptr);
  ASSERT_TRUE(sent->contains("result")) << sent->dump();
  ASSERT_TRUE(batched->contains("result")) << batched->dump();
  ASSERT_FALSE((*sent)["result"]["isError"].get<bool>());
  ASSERT_FALSE((*batched)["result"]["isError"].get<bool>());
  const json& sent_content = (*sent)["result"]["structuredContent"];
  const json& batched_content = (*batched)["result"]["structuredContent"];
  ASSERT_TRUE(sent_content.contains("target_pane_ids"));
  ASSERT_TRUE(batched_content.contains("targets"));
  EXPECT_EQ(sent_content["target_pane_ids"], expected);
  ASSERT_EQ(batched_content["targets"].size(), 1U);
  EXPECT_EQ(batched_content["targets"][0]["resolvedPaneIds"], expected);
}

TEST_F(McpProtocol, RunsTypedReadBatchInDeclaredOrder) {
  const json operations = json::array(
      {{{"tool", "list_sessions"}, {"arguments", json::object()}},
       {{"tool", "get_session_info"}, {"arguments", {{"session", "mcp"}}}}});
  const auto messages = converse_ready(
      socket(), {call("call_read_tools_batch", {{"operations", operations}}, 1)});
  const json* reply = response(messages, 1);
  ASSERT_NE(reply, nullptr);
  ASSERT_TRUE(reply->contains("result")) << reply->dump();
  ASSERT_FALSE((*reply)["result"]["isError"].get<bool>());
  const json& aggregate = (*reply)["result"]["structuredContent"];
  EXPECT_EQ(aggregate["succeeded"], 2);
  EXPECT_EQ(aggregate["failed"], 0);
  EXPECT_EQ(aggregate["onError"], "stop");
  EXPECT_TRUE(aggregate["stoppedAt"].is_null());
  EXPECT_FALSE(aggregate["truncated"].get<bool>());
  EXPECT_EQ(aggregate["truncatedBytes"], 0);
  const json& results = aggregate["results"];
  ASSERT_EQ(results.size(), 2U);
  EXPECT_EQ(results[0]["index"], 0);
  EXPECT_EQ(results[0]["tool"], "list_sessions");
  EXPECT_TRUE(results[0]["success"].get<bool>());
  EXPECT_TRUE(results[0]["error"].is_null());
  EXPECT_FALSE(results[0]["resultTruncated"].get<bool>());
  EXPECT_FALSE(results[0]["result"]["isError"].get<bool>());
  EXPECT_TRUE(results[0]["result"]["content"].is_array());
  EXPECT_TRUE(results[0]["result"]["structuredContent"].contains("sessions"));
  EXPECT_EQ(results[0]["result"]["content"][0]["text"],
            results[0]["result"]["structuredContent"].dump());
  EXPECT_EQ(results[1]["tool"], "get_session_info");
  EXPECT_TRUE(results[1]["result"]["structuredContent"].contains("session"));

  const auto invalid = converse_ready(
      socket(),
      {call("call_read_tools_batch",
            {{"operations", json::array({{{"tool", "get_session_info"},
                                          {"arguments", {{"unknown", "mcp"}}}}})}},
            2)});
  const json* rejected = response(invalid, 2);
  ASSERT_NE(rejected, nullptr);
  EXPECT_EQ((*rejected)["error"]["code"], -32602);
  EXPECT_NE((*rejected)["error"]["message"].get<std::string>().find("unknown argument"),
            std::string::npos);
}

TEST_F(McpProtocol, CapsTheCompleteReadBatchResponseLine) {
  auto server = libtmux::Server::at_socket_path(socket().string());
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  ASSERT_TRUE(server->run({"set-option", "-g", "history-limit", "25000"}).has_value());
  auto sessions = server->sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  ASSERT_FALSE(sessions->empty());
  auto window = sessions->front().new_window("mcp-response-cap");
  ASSERT_TRUE(window.has_value()) << window.error().diagnostic;
  auto panes = window->panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  ASSERT_EQ(panes->size(), 1U);
  const std::string pane_id{panes->front().id()};
  ASSERT_TRUE(panes->front()
                  .send_text("awk 'BEGIN { for (i=0; i<20000; ++i) print "
                             "\"xxxxxxx\" }'")
                  .has_value());
  ASSERT_TRUE(panes->front().send_key("Enter").has_value());
  int history_size = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
  while (history_size < 18'000 && std::chrono::steady_clock::now() < deadline) {
    const auto history =
        server->run({"display-message", "-p", "-t", pane_id, "#{history_size}"});
    ASSERT_TRUE(history.has_value()) << history.error().diagnostic;
    history_size = std::stoi(*history);
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
  }
  ASSERT_GE(history_size, 18'000);

  const std::string identifier(524'000U, 'i');
  const json operation{{"tool", "capture_pane"},
                       {"arguments", {{"paneId", pane_id}, {"history", true}}}};
  const json operations = json::array({operation, operation});
  json request = call("call_read_tools_batch", {{"operations", operations}}, 1);
  request["id"] = identifier;
  const auto finished = libtmux::mcp::test::run_server_until_lines(
      LIBTMUX_MCP_SERVER_PATH, {"--socket-path", socket().string()},
      libtmux::test::current_environment(),
      encode_requests({initialize_request(), initialized_notification(), request}),
      std::chrono::seconds{60}, 2U);
  ASSERT_TRUE(finished.has_value()) << finished.error();
  ASSERT_FALSE(finished->empty());
  ASSERT_EQ(finished->back(), '\n');
  const std::size_t first_end = finished->find('\n');
  ASSERT_NE(first_end, std::string::npos);
  const std::size_t response_start = first_end + 1U;
  ASSERT_EQ(finished->find('\n', response_start), finished->size() - 1U);
  const std::string_view response_line{*finished};
  const std::string_view call_line =
      response_line.substr(response_start, finished->size() - response_start - 1U);

  EXPECT_LE(call_line.size() + 1U, 1'000'000U);
  const json reply = json::parse(call_line);
  EXPECT_EQ(reply["id"], identifier);
  const json& aggregate = reply["result"]["structuredContent"];
  ASSERT_EQ(aggregate["results"].size(), 2U);
  EXPECT_TRUE(aggregate["truncated"].get<bool>());
  for (std::size_t index = 0; index < 2U; ++index) {
    EXPECT_EQ(aggregate["results"][index]["index"], index);
    EXPECT_TRUE(aggregate["results"][index]["resultTruncated"].get<bool>());
    EXPECT_TRUE(aggregate["results"][index]["result"].is_null());
  }
}

TEST_F(McpProtocol, RejectsAnOversizedRequestIdBeforeToolDispatch) {
  auto server = libtmux::Server::at_socket_path(socket().string());
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  auto panes = server->panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  ASSERT_FALSE(panes->empty());
  const std::string pane_id{panes->front().id()};
  const std::string marker = "oversized-request-id-must-not-run";
  json rejected = call("paste_text", {{"paneId", pane_id}, {"text", marker}}, 1);
  rejected["id"] = std::string(1'000'000U, 'i');

  const auto refused = converse_ready(socket(), {std::move(rejected)});
  const json* error = response(refused, nullptr);
  ASSERT_NE(error, nullptr);
  EXPECT_EQ((*error)["error"]["code"], -32600);
  EXPECT_EQ((*error)["error"]["message"], "request id exceeds 524288 bytes");

  const auto checked =
      converse_ready(socket(), {call("capture_pane", {{"paneId", pane_id}}, 2)});
  const json* captured = response(checked, 2);
  ASSERT_NE(captured, nullptr);
  EXPECT_EQ((*captured)["result"]["structuredContent"]["text"].get<std::string>().find(
                marker),
            std::string::npos);
}

TEST_F(McpProtocol, ExpandsOnlyBoundedValidatedVariableNames) {
  const auto messages = converse_ready(
      socket(),
      {call("get_tmux_variables",
            {{"names", json::array({"pane_id", "pane_title"})}, {"paneId", "mcp"}}, 1),
       call("get_tmux_variables", {{"names", json::array({"pane_id", "#(id)"})}}, 2)});
  const json* expanded = response(messages, 1);
  const json* rejected = response(messages, 2);
  ASSERT_NE(expanded, nullptr);
  ASSERT_NE(rejected, nullptr);
  ASSERT_TRUE(expanded->contains("result")) << expanded->dump();
  EXPECT_FALSE((*expanded)["result"]["isError"].get<bool>());
  const json& values = (*expanded)["result"]["structuredContent"]["values"];
  EXPECT_TRUE(values.contains("pane_id"));
  EXPECT_TRUE(values.contains("pane_title"));
  EXPECT_EQ((*rejected)["error"]["code"], -32602);
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
      call("paste_text",
           {{"paneId", "mcp"}, {"text", "printf 'mcp-%s\\n' 'stream-marker'\n"}}, 2)};
  const std::vector<json> search{
      call("search_panes", {{"pattern", "mcp-stream-marker"}}, 3)};

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

// A thread-sanitized build pays for every synchronising operation, so a
// wall-clock budget written for an ordinary one measures the sanitizer.
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
inline constexpr bool sanitized_build = true;
#else
inline constexpr bool sanitized_build = false;
#endif
#else
inline constexpr bool sanitized_build = false;
#endif

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
  // What this asserts is that end of input ends the wait rather than letting
  // it run its sixty seconds. The wall clock it takes to do that is not the
  // same under an instrumented build: ThreadSanitizer prices every atomic and
  // every lock, and the same exchange measures the same on an ordinary build
  // whichever transport runs it. So the bound is on promptness, and the number
  // follows the build.
  EXPECT_LT(elapsed,
            sanitized_build ? std::chrono::seconds{8} : std::chrono::seconds{4});
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
  EXPECT_EQ(catalog["tools"].size(), 41U);
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
      json::array({call("paste_text", {{"paneId", "mcp"}, {"text", marker}}, 7),
                   call("paste_text", {{"paneId", "mcp"}, {"text", marker}}, 7)});
  const auto messages =
      converse(socket(), {initialize_request("2025-03-26"), initialized_notification(),
                          batch, call("capture_pane", {{"paneId", "mcp"}}, 8)});
  const json* first = response(messages, 7);
  const json* captured = response(messages, 8);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(captured, nullptr);
  ASSERT_GE(messages.size(), 2U);
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
      {modern_call("capture_pane", {{"paneId", 99}}, 1),
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
                     call("capture_pane", {{"paneId", 99}}, 1),
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
  json compatible = call("capture_pane", {{"paneId", 99}}, 1, "client-progress");
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
  EXPECT_EQ((*accepted)["error"]["message"], "argument paneId must be a string");
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

TEST(McpProtocolTmux, UsesTheExactSocketPathEnvironment) {
  auto started = ScopedTmuxServer::start(ScopedTmuxServerOptions{
      .session_name = "mcp-env",
      .socket_namespace = SocketNamespace::consumer("mcp-env")});
  ASSERT_TRUE(started.has_value()) << started.error();
  auto environment = started->child_environment();
  libtmux::test::erase_environment(environment, "TMUX");
  libtmux::test::set_environment(environment, "LIBTMUX_SOCKET_PATH",
                                 started->socket_path().string());
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

TEST(McpProtocolCli, EstablishesDefaultMinimalDaemonBeforeFreezingProvenance) {
  const std::filesystem::path configuration =
      libtmux::mcp::server::minimal_configuration_path(LIBTMUX_MCP_SERVER_PATH);
  std::ifstream input{configuration};
  ASSERT_TRUE(input) << configuration;
  const std::string contents{std::istreambuf_iterator<char>{input},
                             std::istreambuf_iterator<char>{}};
  ASSERT_FALSE(input.bad());
  EXPECT_NE(contents.find("exit-empty off"), std::string::npos);
  EXPECT_EQ(contents.find("run-shell"), std::string::npos);

  auto sentinel = ScopedTmuxServer::start(ScopedTmuxServerOptions{
      .mode = SocketMode::Name,
      .session_name = "provenance-sentinel",
      .socket_namespace = SocketNamespace::consumer("mcp-prov")});
  ASSERT_TRUE(sentinel.has_value()) << sentinel.error();
  const auto private_permissions =
      std::filesystem::status(sentinel->tmux_tmpdir()).permissions();
  EXPECT_EQ(private_permissions & (std::filesystem::perms::group_all |
                                   std::filesystem::perms::others_all),
            std::filesystem::perms::none);
  const libtmux::test::EnvironmentGuard tmux_tmpdir{"TMUX_TMPDIR",
                                                    sentinel->tmux_tmpdir().string()};
  libtmux::mcp::server::CliOptions options;
  // tmux spends `$TMUX_TMPDIR/tmux-<uid>/` before this name, so it stays short
  // for the same reason the socket above does.
  options.value = "prov";
  auto opened = libtmux::mcp::server::open_server(options);
  ASSERT_TRUE(opened.has_value()) << opened.error();
  const std::filesystem::path selected_socket{opened->server.socket_path()};
  EXPECT_EQ(selected_socket.parent_path().parent_path(), sentinel->tmux_tmpdir());
  EXPECT_FALSE(opened->server_pre_existing);
  EXPECT_TRUE(opened->teardown_enabled_by_default);
  EXPECT_TRUE(opened->owns_daemon);
  EXPECT_EQ(opened->socket_provenance, "default-dedicated");
  EXPECT_EQ(opened->configuration_provenance, "minimal");
  const auto environment = opened->server.run({"show-environment", "-g"});
  const auto alive = opened->server.run({"show-options", "-sqv", "exit-empty"});
  const auto killed = opened->server.kill();

  const auto stopped_by = std::chrono::steady_clock::now() + std::chrono::seconds{1};
  while (killed.has_value() && opened->server.is_alive(std::chrono::milliseconds{50}) &&
         std::chrono::steady_clock::now() < stopped_by) {
    std::this_thread::yield();
  }
  const bool stopped =
      killed.has_value() && !opened->server.is_alive(std::chrono::milliseconds{50});
  std::error_code cleanup_error;
  const bool removed =
      stopped && std::filesystem::remove(selected_socket, cleanup_error);

  ASSERT_TRUE(environment.has_value()) << environment.error().diagnostic;
  EXPECT_EQ(environment->find("LIBTMUX_MCP_OWNER="), std::string::npos);
  EXPECT_TRUE(alive.has_value()) << alive.error().diagnostic;
  EXPECT_TRUE(killed.has_value()) << killed.error().diagnostic;
  EXPECT_TRUE(stopped);
  EXPECT_TRUE(removed);
  EXPECT_FALSE(cleanup_error) << cleanup_error.message();
  EXPECT_FALSE(std::filesystem::exists(selected_socket));
  EXPECT_TRUE(sentinel->is_alive());
  const auto sibling =
      libtmux::Server::at_socket_path(sentinel->socket_path().string());
  ASSERT_TRUE(sibling.has_value()) << sibling.error().diagnostic;
  EXPECT_TRUE(sibling->is_alive());
}

TEST(McpProtocolCli, StopsTheAuthenticatedDefaultDaemonWhenStdioCloses) {
  auto fixture = ScopedTmuxServer::start(ScopedTmuxServerOptions{
      .mode = SocketMode::Name,
      .session_name = "cleanup-namespace",
      .socket_namespace = SocketNamespace::consumer("mcp-stop")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  auto environment = fixture->child_environment();
  libtmux::test::erase_environment(environment, "TMUX");
  for (const std::string_view name :
       {"LIBTMUX_SOCKET", "LIBTMUX_SOCKET_PATH", "LIBTMUX_TMUX_CONFIG",
        "LIBTMUX_TOOLSETS", "LIBTMUX_TOOLS", "LIBTMUX_EXCLUDE_TOOLS"}) {
    libtmux::test::erase_environment(environment, name);
  }

  const auto finished = libtmux::mcp::test::run_server(
      LIBTMUX_MCP_SERVER_PATH, {}, environment, {}, std::chrono::seconds{5});
  auto server = libtmux::Server::at_socket_path(
      (fixture->socket_path().parent_path() / "libtmux-mcp").string());
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  const bool still_alive =
      server
          ->run({"show-options", "-sqv", "exit-empty"}, std::chrono::milliseconds{250})
          .has_value();
  if (still_alive) {
    static_cast<void>(server->kill());
  }

  ASSERT_TRUE(finished.has_value()) << finished.error();
  EXPECT_FALSE(still_alive);
}

TEST(McpProtocolCli, DoesNotClaimAnExistingDedicatedDaemon) {
  auto fixture = ScopedTmuxServer::start(ScopedTmuxServerOptions{
      .mode = SocketMode::Name,
      .session_name = "provenance-holder",
      .socket_namespace = SocketNamespace::consumer("mcp-own")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  auto environment = fixture->child_environment();
  for (const std::string_view name :
       {"LIBTMUX_SOCKET", "LIBTMUX_SOCKET_PATH", "LIBTMUX_TMUX_CONFIG",
        "LIBTMUX_TOOLSETS", "LIBTMUX_TOOLS", "LIBTMUX_EXCLUDE_TOOLS"}) {
    libtmux::test::erase_environment(environment, name);
  }
  const std::filesystem::path socket =
      fixture->socket_path().parent_path() / "libtmux-mcp";
  auto server = libtmux::Server::startable_at_socket_path(
      socket.string(),
      libtmux::mcp::server::minimal_configuration_path(LIBTMUX_MCP_SERVER_PATH));
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  const auto started = server->run({"start-server"});
  ASSERT_TRUE(started.has_value()) << started.error().diagnostic;
  const auto messages =
      converse_with({}, environment,
                    {initialize_request(), initialized_notification(),
                     json{{"jsonrpc", "2.0"},
                          {"id", 1},
                          {"method", "resources/read"},
                          {"params", {{"uri", "tmux://capabilities"}}}}});
  const auto killed = server->kill();

  ASSERT_TRUE(killed.has_value()) << killed.error().diagnostic;
  const json* read = response(messages, 1);
  ASSERT_NE(read, nullptr);
  const json document =
      json::parse((*read)["result"]["contents"][0]["text"].get<std::string>());
  EXPECT_EQ(document["socket"]["serverState"], "existing");
  EXPECT_EQ(document["socket"]["configurationProvenance"], "unknown");
  EXPECT_EQ(document["toolCount"], 41);
}

TEST(McpProtocolCli, PublishesStaticEffectiveCapabilitiesInBothEras) {
  auto environment = libtmux::test::current_environment();
  libtmux::test::set_environment(environment, "LIBTMUX_TOOLSETS", "inspect,execute");
  libtmux::test::set_environment(environment, "LIBTMUX_TOOLS",
                                 "create_session,paste_text");
  libtmux::test::set_environment(environment, "LIBTMUX_EXCLUDE_TOOLS",
                                 "list_sessions,paste_text");
  const json legacy_list{
      {"jsonrpc", "2.0"}, {"id", "legacy-resources"}, {"method", "resources/list"}};
  const json legacy_read{{"jsonrpc", "2.0"},
                         {"id", "legacy-read"},
                         {"method", "resources/read"},
                         {"params", {{"uri", "tmux://capabilities"}}}};
  const json legacy_tools{
      {"jsonrpc", "2.0"}, {"id", "legacy-tools"}, {"method", "tools/list"}};
  const json excluded_call = call("paste_text", json::object(), 7);
  const auto legacy = converse_with(
      {"--socket-name", "libtmux-cxx-capabilities-legacy-no-dispatch"}, environment,
      {initialize_request(), initialized_notification(), legacy_list, legacy_read,
       legacy_tools, excluded_call});

  const json* initialized = response(legacy, "initialize");
  const json* listed = response(legacy, "legacy-resources");
  const json* read = response(legacy, "legacy-read");
  const json* tools = response(legacy, "legacy-tools");
  const json* excluded = response(legacy, 7);
  ASSERT_NE(initialized, nullptr);
  ASSERT_NE(listed, nullptr);
  ASSERT_NE(read, nullptr);
  ASSERT_NE(tools, nullptr);
  ASSERT_NE(excluded, nullptr);
  ASSERT_TRUE((*initialized)["result"]["capabilities"].contains("resources"));
  EXPECT_EQ((*initialized)["result"]["capabilities"]["resources"],
            json({{"listChanged", false}, {"subscribe", false}}));
  ASSERT_TRUE(listed->contains("result"));
  ASSERT_EQ((*listed)["result"]["resources"].size(), 1U);
  EXPECT_EQ((*listed)["result"]["resources"][0]["uri"], "tmux://capabilities");
  ASSERT_TRUE(read->contains("result"));
  ASSERT_EQ((*read)["result"]["contents"].size(), 1U);
  const json document =
      json::parse((*read)["result"]["contents"][0]["text"].get<std::string>());
  EXPECT_EQ(document["schemaVersion"], 1);
  EXPECT_TRUE(document["frozen"].get<bool>());
  ASSERT_TRUE(document.contains("toolsets"));
  ASSERT_TRUE(document.contains("includedTools"));
  ASSERT_TRUE(document.contains("excludedTools"));
  ASSERT_TRUE(document.contains("socket"));
  EXPECT_EQ(document["toolsets"], json::array({"inspect", "execute"}));
  EXPECT_EQ(document["includedTools"], json::array({"create_session", "paste_text"}));
  EXPECT_EQ(document["excludedTools"], json::array({"list_sessions", "paste_text"}));
  EXPECT_EQ(document["toolCount"], 25);
  EXPECT_EQ(document["hostCommandTools"], 0);
  EXPECT_EQ(document["toolFilteringBoundary"], "interface-shaping-not-authorization");
  EXPECT_EQ(document["executionAuthority"], "tmux-user");
  EXPECT_EQ(document["operatingSystemBoundary"], "none");
  EXPECT_EQ(document["socket"]["selector"],
            "name:libtmux-cxx-capabilities-legacy-no-dispatch");
  EXPECT_EQ(document["socket"]["selectionProvenance"], "operator-current");
  EXPECT_EQ(document["socket"]["serverState"], "absent");
  EXPECT_EQ(document["socket"]["configurationProvenance"], "unknown");
  EXPECT_EQ(document["socket"]["namespaceBoundary"], "tmux-objects-only");
  EXPECT_EQ(document["boundary"], json({{"oneSocketPerProcess", true},
                                        {"perCallSocketSelection", false},
                                        {"hostCommandExecution", false},
                                        {"dynamicResources", false}}));
  ASSERT_TRUE(document.contains("connection"));
  EXPECT_EQ(document["connection"]["socketSelector"], document["socket"]["selector"]);
  EXPECT_EQ(document["connection"]["socketProvenance"],
            document["socket"]["selectionProvenance"]);
  EXPECT_EQ(document["connection"]["serverState"], document["socket"]["serverState"]);
  EXPECT_EQ(document["connection"]["configurationProvenance"],
            document["socket"]["configurationProvenance"]);
  ASSERT_TRUE(document["connection"]["resolvedSocketPath"].is_string());
  EXPECT_FALSE(document["connection"]["resolvedSocketPath"].get<std::string>().empty());
  ASSERT_TRUE(document["connection"]["attachCommand"].is_string());
  EXPECT_NE(document["connection"]["attachCommand"].get<std::string>().find(" -N -S '"),
            std::string::npos);

  const std::vector<std::string> expected{"list_windows",
                                          "list_panes",
                                          "get_server_info",
                                          "get_session_info",
                                          "get_window_info",
                                          "get_pane_info",
                                          "capture_pane",
                                          "capture_since",
                                          "snapshot_pane",
                                          "search_panes",
                                          "find_pane_by_position",
                                          "wait_for_text",
                                          "get_tmux_variables",
                                          "show_option",
                                          "show_environment",
                                          "show_hooks",
                                          "call_read_tools_batch",
                                          "create_session",
                                          "create_window",
                                          "split_window",
                                          "respawn_pane",
                                          "run_shell_command",
                                          "send_keys",
                                          "send_keys_batch",
                                          "set_synchronize_panes"};
  std::vector<std::string> listed_names;
  for (const json& tool : (*tools)["result"]["tools"]) {
    listed_names.push_back(tool["name"].get<std::string>());
  }
  std::vector<std::string> reported_names;
  for (const json& tool : document["tools"]) {
    reported_names.push_back(tool["name"].get<std::string>());
  }
  EXPECT_EQ(listed_names, expected);
  EXPECT_EQ(reported_names, expected);
  for (const json& listed_tool : (*tools)["result"]["tools"]) {
    const auto row = std::ranges::find_if(document["tools"], [&](const json& item) {
      return item["name"] == listed_tool["name"];
    });
    ASSERT_NE(row, document["tools"].end()) << listed_tool["name"];
    ASSERT_TRUE(listed_tool.contains("_meta")) << listed_tool["name"];
    EXPECT_EQ(listed_tool["_meta"]["com.git-pull.libtmux-mcp/capability"], *row)
        << listed_tool["name"];
  }
  for (const json& tool : document["tools"]) {
    EXPECT_FALSE(tool.contains("inputSinks")) << tool["name"];
    EXPECT_FALSE(tool.contains("tmuxFormatControls")) << tool["name"];
    ASSERT_TRUE(tool["inputLiteralization"].is_object()) << tool["name"];
    for (const auto& [field, control] : tool["inputLiteralization"].items()) {
      EXPECT_TRUE(tool["inputSchema"]["properties"].contains(field))
          << tool["name"] << '.' << field;
      EXPECT_TRUE(control == "double-hash-once" || control == "validated-variable-name")
          << tool["name"] << '.' << field;
    }
  }
  const auto sent_keys = std::ranges::find_if(
      document["tools"], [](const json& tool) { return tool["name"] == "send_keys"; });
  ASSERT_NE(sent_keys, document["tools"].end());
  EXPECT_EQ((*sent_keys)["toolset"], "execute");
  EXPECT_EQ((*sent_keys)["processReach"], "pane-input");
  EXPECT_EQ((*sent_keys)["tmuxEffects"], json::array({"observe", "change"}));
  EXPECT_EQ((*sent_keys)["outputClasses"], json::array({"tmux-metadata"}));
  EXPECT_FALSE((*sent_keys)["mayExposeSecrets"].get<bool>());
  EXPECT_TRUE((*sent_keys)["mayReturnUntrustedContent"].get<bool>());
  EXPECT_TRUE((*sent_keys)["annotations"]["destructiveHint"].get<bool>());
  EXPECT_TRUE((*sent_keys)["inputSchema"]["properties"].contains("keys"));
  const auto synchronized =
      std::ranges::find_if(document["tools"], [](const json& tool) {
        return tool["name"] == "set_synchronize_panes";
      });
  ASSERT_NE(synchronized, document["tools"].end());
  ASSERT_TRUE((*synchronized).contains("amplifiesFutureInput"));
  EXPECT_TRUE((*synchronized)["amplifiesFutureInput"].get<bool>());
  EXPECT_EQ((*synchronized)["description"],
            "Change tmux state; no client-supplied executable input. Set the "
            "inherited window synchronize-panes default; pane-level overrides "
            "determine effective synchronized input membership.");
  for (const json& tool : document["tools"]) {
    ASSERT_TRUE(tool.contains("amplifiesFutureInput")) << tool["name"];
    EXPECT_EQ(tool["amplifiesFutureInput"].get<bool>(),
              tool["name"] == "set_synchronize_panes")
        << tool["name"];
  }
  const auto create_session =
      std::ranges::find_if(document["tools"], [](const json& tool) {
        return tool["name"] == "create_session";
      });
  ASSERT_NE(create_session, document["tools"].end());
  ASSERT_TRUE((*create_session).contains("inputLiteralization"));
  EXPECT_EQ((*create_session)["inputLiteralization"]["startDirectory"],
            "double-hash-once");
  EXPECT_EQ((*create_session)["inputLiteralization"]["name"], "double-hash-once");
  EXPECT_EQ((*create_session)["inputLiteralization"]["windowName"], "double-hash-once");
  for (const auto& [tool_name, field_name] : {std::pair{"create_window", "name"}}) {
    const auto row = std::ranges::find_if(
        document["tools"], [&](const json& tool) { return tool["name"] == tool_name; });
    ASSERT_NE(row, document["tools"].end()) << tool_name;
    EXPECT_EQ((*row)["inputLiteralization"][field_name], "double-hash-once")
        << tool_name;
  }
  const auto variables = std::ranges::find_if(document["tools"], [](const json& tool) {
    return tool["name"] == "get_tmux_variables";
  });
  ASSERT_NE(variables, document["tools"].end());
  EXPECT_EQ((*variables)["inputLiteralization"]["names"], "validated-variable-name");
  const auto read_batch = std::ranges::find_if(document["tools"], [](const json& tool) {
    return tool["name"] == "call_read_tools_batch";
  });
  ASSERT_NE(read_batch, document["tools"].end());
  const json expected_nested = json::array(
      {"capture_pane", "capture_since", "find_pane_by_position", "get_pane_info",
       "get_server_info", "get_session_info", "get_tmux_variables", "get_window_info",
       "list_panes", "list_windows", "search_panes", "show_environment", "show_hooks",
       "show_option", "snapshot_pane"});
  const auto expected_nested_names = expected_nested.get<std::vector<std::string>>();
  EXPECT_EQ((*read_batch)["nestedAuthority"], expected_nested);
  const json& alternatives =
      (*read_batch)["inputSchema"]["properties"]["operations"]["items"]["oneOf"];
  ASSERT_EQ(alternatives.size(), expected_nested.size());
  for (const json& alternative : alternatives) {
    const std::string nested_name =
        alternative["properties"]["tool"]["const"].get<std::string>();
    const auto expected_name = std::ranges::find(expected_nested_names, nested_name);
    EXPECT_NE(expected_name, expected_nested_names.end()) << nested_name;
    EXPECT_FALSE(
        alternative["properties"]["arguments"]["additionalProperties"].get<bool>())
        << nested_name;
  }
  EXPECT_EQ((*read_batch)["inputSchema"]["properties"]["onError"]["enum"],
            json::array({"continue", "stop"}));
  EXPECT_EQ((*excluded)["error"]["message"], "unknown tool: paste_text");

  const auto modern =
      converse_with({"--socket-name", "libtmux-cxx-capabilities-modern-no-dispatch"},
                    std::move(environment),
                    {modern_request("server/discover", "discover"),
                     modern_request("resources/list", "modern-resources"),
                     modern_request("resources/read", "modern-read",
                                    {{"uri", "tmux://capabilities"}})});
  const json* discovered = response(modern, "discover");
  const json* modern_list = response(modern, "modern-resources");
  const json* modern_read = response(modern, "modern-read");
  ASSERT_NE(discovered, nullptr);
  ASSERT_NE(modern_list, nullptr);
  ASSERT_NE(modern_read, nullptr);
  ASSERT_TRUE((*discovered)["result"]["capabilities"].contains("resources"));
  EXPECT_EQ((*discovered)["result"]["capabilities"]["resources"],
            json({{"listChanged", false}, {"subscribe", false}}));
  ASSERT_TRUE(modern_list->contains("result"));
  ASSERT_TRUE(modern_read->contains("result"));
  EXPECT_EQ((*modern_list)["result"]["resultType"], "complete");
  EXPECT_EQ((*modern_read)["result"]["resultType"], "complete");
  const json modern_document =
      json::parse((*modern_read)["result"]["contents"][0]["text"].get<std::string>());
  std::vector<std::string> modern_names;
  for (const json& tool : modern_document["tools"]) {
    modern_names.push_back(tool["name"].get<std::string>());
  }
  EXPECT_EQ(modern_names, expected);
}

TEST(McpProtocolCli, RejectsInvalidPolicyBeforeOpeningTmux) {
  struct InvalidPolicy {
    std::string name;
    std::string value;
  };
  const std::vector<InvalidPolicy> invalid{
      {"LIBTMUX_TOOLSETS", "inspect,,execute"},
      {"LIBTMUX_TOOLSETS", "unknown"},
      {"LIBTMUX_TOOLS", ""},
      {"LIBTMUX_TOOLS", "unknown"},
      {"LIBTMUX_EXCLUDE_TOOLS", "capture_pane,"},
      {"LIBTMUX_EXCLUDE_TOOLS", "unknown"},
      {"LIBTMUX_SOCKET", ""},
      {"LIBTMUX_SOCKET_PATH", ""},
      {"LIBTMUX_TMUX_CONFIG", ""},
      {"LIBTMUX_TMUX_CONFIG", "relative.conf"},
      {"LIBTMUX_SAFETY", "read-only"},
  };
  for (const auto& policy : invalid) {
    auto environment = libtmux::test::current_environment();
    libtmux::test::erase_environment(environment, "TMUX");
    libtmux::test::set_environment(environment, policy.name, policy.value);
    const auto finished = libtmux::mcp::test::run_server(LIBTMUX_MCP_SERVER_PATH, {},
                                                         std::move(environment), {},
                                                         std::chrono::seconds{5});
    ASSERT_FALSE(finished.has_value()) << policy.name << '=' << policy.value;
    EXPECT_NE(finished.error().find("exited with status 2"), std::string::npos)
        << policy.name << '=' << policy.value;
  }
}

TEST(McpProtocolCli, UsesSeparateSocketEnvironmentVariables) {
  auto environment = libtmux::test::current_environment();
  libtmux::test::erase_environment(environment, "TMUX");
  libtmux::test::erase_environment(environment, "LIBTMUX_SOCKET");
  libtmux::test::set_environment(environment, "LIBTMUX_SOCKET_PATH",
                                 "/tmp/libtmux-cxx-env-path-no-dispatch.sock");
  const auto messages =
      converse_with({}, environment,
                    {initialize_request(), initialized_notification(),
                     json{{"jsonrpc", "2.0"},
                          {"id", 1},
                          {"method", "resources/read"},
                          {"params", {{"uri", "tmux://capabilities"}}}}});
  const json* read = response(messages, 1);
  ASSERT_NE(read, nullptr);
  const json document =
      json::parse((*read)["result"]["contents"][0]["text"].get<std::string>());
  EXPECT_EQ(document["socket"]["selector"],
            "path:/tmp/libtmux-cxx-env-path-no-dispatch.sock");
  EXPECT_EQ(document["socket"]["selectionProvenance"], "operator-current");

  libtmux::test::set_environment(environment, "LIBTMUX_SOCKET", "conflict");
  const auto conflicting = libtmux::mcp::test::run_server(
      LIBTMUX_MCP_SERVER_PATH, {}, std::move(environment), {}, std::chrono::seconds{5});
  ASSERT_FALSE(conflicting.has_value());
  EXPECT_NE(conflicting.error().find("exited with status 2"), std::string::npos);
}

TEST(McpProtocolCli, RetainsNestedReadsForAggregateOnlySelection) {
  auto environment = libtmux::test::current_environment();
  libtmux::test::erase_environment(environment, "TMUX");
  libtmux::test::set_environment(environment, "LIBTMUX_TOOLSETS", "");
  libtmux::test::set_environment(environment, "LIBTMUX_TOOLS", "call_read_tools_batch");
  libtmux::test::set_environment(environment, "LIBTMUX_EXCLUDE_TOOLS", "capture_pane");
  const json operations =
      json::array({{{"tool", "list_sessions"}, {"arguments", json::object()}}});
  const auto messages = converse_with(
      {"--socket-name", "libtmux-cxx-aggregate-only-no-dispatch"}, environment,
      {initialize_request(), initialized_notification(),
       json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}},
       call("call_read_tools_batch", {{"operations", operations}}, 2)});
  const json* listed = response(messages, 1);
  const json* called = response(messages, 2);
  ASSERT_NE(listed, nullptr);
  ASSERT_NE(called, nullptr);
  ASSERT_EQ((*listed)["result"]["tools"].size(), 1U);
  const json& batch = (*listed)["result"]["tools"][0];
  EXPECT_EQ(batch["name"], "call_read_tools_batch");
  const json& alternatives =
      batch["inputSchema"]["properties"]["operations"]["items"]["oneOf"];
  EXPECT_EQ(alternatives.size(), 15U);
  const auto names =
      alternatives | std::views::transform([](const json& alternative) {
        return alternative["properties"]["tool"]["const"].get<std::string>();
      });
  EXPECT_NE(std::ranges::find(names, "list_sessions"), names.end());
  EXPECT_EQ(std::ranges::find(names, "capture_pane"), names.end());
  ASSERT_TRUE(called->contains("result")) << called->dump();
  ASSERT_FALSE((*called)["result"]["isError"].get<bool>());
  EXPECT_EQ((*called)["result"]["structuredContent"]["results"][0]["tool"],
            "list_sessions");
}

TEST(McpProtocolCli, MakesAReadBatchWithNoNestedAuthorityUnsatisfiable) {
  auto environment = libtmux::test::current_environment();
  libtmux::test::erase_environment(environment, "TMUX");
  libtmux::test::set_environment(environment, "LIBTMUX_TOOLSETS", "");
  libtmux::test::set_environment(environment, "LIBTMUX_TOOLS", "call_read_tools_batch");
  libtmux::test::set_environment(
      environment, "LIBTMUX_EXCLUDE_TOOLS",
      "list_sessions,list_windows,list_panes,get_server_info,get_session_info,"
      "get_window_info,get_pane_info,capture_pane,capture_since,snapshot_pane,"
      "search_panes,find_pane_by_position,get_tmux_variables,show_option,"
      "show_environment,show_hooks");
  const auto messages = converse_with(
      {"--socket-name", "libtmux-cxx-zero-authority-no-dispatch"}, environment,
      {initialize_request(), initialized_notification(),
       json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}});
  const json* listed = response(messages, 1);
  ASSERT_NE(listed, nullptr);
  const json& tools = (*listed)["result"]["tools"];
  ASSERT_EQ(tools.size(), 1U);
  const json& capability = tools[0]["_meta"]["com.git-pull.libtmux-mcp/capability"];
  EXPECT_EQ(capability["nestedAuthority"], json::array());
  EXPECT_EQ(capability["tmuxEffects"], json::array({"observe"}));
  EXPECT_EQ(capability["outputClasses"], json::array());
  const json& operations = tools[0]["inputSchema"]["properties"]["operations"];
  EXPECT_EQ(operations["minItems"], 1);
  EXPECT_EQ(operations["items"], false);
}

TEST(McpProtocolCli, ExplicitSocketDefaultsExcludeTeardown) {
  auto environment = libtmux::test::current_environment();
  libtmux::test::erase_environment(environment, "TMUX");
  libtmux::test::erase_environment(environment, "LIBTMUX_TOOLSETS");
  libtmux::test::erase_environment(environment, "LIBTMUX_TOOLS");
  libtmux::test::erase_environment(environment, "LIBTMUX_EXCLUDE_TOOLS");
  const auto messages = converse_with(
      {"--socket-name", "libtmux-cxx-explicit-default-no-dispatch"}, environment,
      {initialize_request(), initialized_notification(),
       json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}},
       json{{"jsonrpc", "2.0"},
            {"id", 2},
            {"method", "resources/read"},
            {"params", {{"uri", "tmux://capabilities"}}}}});
  const json* listed = response(messages, 1);
  const json* read = response(messages, 2);
  ASSERT_NE(listed, nullptr);
  ASSERT_NE(read, nullptr);
  const json& tools = (*listed)["result"]["tools"];
  EXPECT_EQ(tools.size(), 41U);
  EXPECT_EQ(std::ranges::find(
                tools, "kill_session",
                [](const json& tool) { return tool["name"].get<std::string>(); }),
            tools.end());
  const json document =
      json::parse((*read)["result"]["contents"][0]["text"].get<std::string>());
  EXPECT_EQ(document["socket"]["selectionProvenance"], "operator-current");
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

TEST_F(McpProtocol, KeepsBatchIdsReservedThroughAggregateBackpressure) {
  const std::string huge_name(2U * 1024U * 1024U, 'x');
  const std::string initialize = initialize_request("2025-03-26").dump() + '\n';
  const std::string initialized = initialized_notification().dump() + '\n';
  const std::string first =
      json::array({call(huge_name, json::object(), 9),
                   call("wait_for_text",
                        {{"target", "mcp"},
                         {"text", "batch-backpressure-never-appears"},
                         {"timeout_ms", 5000}},
                        10, "batch-backpressure")})
          .dump() +
      '\n';
  const std::string cancel =
      json{{"jsonrpc", "2.0"},
           {"method", "notifications/cancelled"},
           {"params", {{"requestId", 10}, {"reason", "barrier reached"}}}}
          .dump() +
      '\n';
  const std::string duplicate =
      json{{"jsonrpc", "2.0"}, {"id", 9}, {"method", "ping"}}.dump() + '\n';
  auto replies = libtmux::mcp::test::run_backpressure_probe(
      LIBTMUX_MCP_SERVER_PATH, {"--socket-path", socket().string()},
      libtmux::test::current_environment(), initialize, initialized, first, duplicate,
      std::chrono::seconds{15}, 1U, cancel);
  ASSERT_TRUE(replies.has_value()) << replies.error();
  ASSERT_EQ(replies->size(), 4U);

  const json barrier = json::parse((*replies)[1]);
  ASSERT_EQ(barrier["method"], "notifications/progress");
  EXPECT_EQ(barrier["params"]["progressToken"], "batch-backpressure");

  const json reply_a = json::parse((*replies)[2]);
  const json reply_b = json::parse((*replies)[3]);
  const json& aggregate = reply_a.is_array() ? reply_a : reply_b;
  const json& duplicate_reply = reply_a.is_array() ? reply_b : reply_a;
  ASSERT_TRUE(aggregate.is_array());
  ASSERT_EQ(aggregate.size(), 1U);
  EXPECT_EQ(aggregate[0]["id"], 9);
  EXPECT_EQ(duplicate_reply["id"], 9);
  EXPECT_EQ(duplicate_reply["error"]["message"], "request id is already in flight");
}

TEST(McpProtocolCli, UsesTheProductDedicatedDefaultRoute) {
  auto fixture = ScopedTmuxServer::start(ScopedTmuxServerOptions{
      .mode = SocketMode::Name,
      .session_name = "default-route-holder",
      .socket_namespace = SocketNamespace::consumer("mcp-dft")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  auto environment = fixture->child_environment();
  const auto messages =
      converse_with({}, std::move(environment),
                    {initialize_request(), initialized_notification(),
                     json{{"jsonrpc", "2.0"},
                          {"id", 1},
                          {"method", "resources/read"},
                          {"params", {{"uri", "tmux://capabilities"}}}}});
  auto server = libtmux::Server::at_socket_path(
      (fixture->socket_path().parent_path() / "libtmux-mcp").string());
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  EXPECT_FALSE(
      server
          ->run({"show-options", "-sqv", "exit-empty"}, std::chrono::milliseconds{250})
          .has_value());
  const json* read = response(messages, 1);
  ASSERT_NE(read, nullptr);
  const json document =
      json::parse((*read)["result"]["contents"][0]["text"].get<std::string>());
  EXPECT_EQ(document["socket"]["serverState"], "created");
  EXPECT_EQ(document["socket"]["configurationProvenance"], "minimal");
  EXPECT_EQ(document["socket"]["selectionProvenance"], "default-dedicated");
  EXPECT_EQ(document["toolCount"], 45);
}

TEST(McpProtocolCli, DoesNotUseAnInvalidInheritedRoute) {
  auto fixture = ScopedTmuxServer::start(ScopedTmuxServerOptions{
      .mode = SocketMode::Name,
      .session_name = "invalid-route-holder",
      .socket_namespace = SocketNamespace::consumer("mcp-inv")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  auto environment = fixture->child_environment();
  libtmux::test::set_environment(environment, "TMUX", "");
  const auto finished = libtmux::mcp::test::run_server(
      LIBTMUX_MCP_SERVER_PATH, {}, std::move(environment), {}, std::chrono::seconds{5});
  ASSERT_TRUE(finished.has_value());
  EXPECT_EQ(*finished, "");
  auto server = libtmux::Server::at_socket_path(
      (fixture->socket_path().parent_path() / "libtmux-mcp").string());
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  EXPECT_FALSE(
      server
          ->run({"show-options", "-sqv", "exit-empty"}, std::chrono::milliseconds{250})
          .has_value());
}

} // namespace
