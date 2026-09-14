#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "backend.hpp"
#include "libtmux/server.hpp"
#include "libtmux/testing/scoped_server.hpp"
#include "libtmux_consumers/mcp.hpp"
#include "schema.hpp"

namespace {

using json = nlohmann::json;
using libtmux::Server;
using libtmux::mcp::Arguments;
using libtmux::mcp::default_tools;
using libtmux::mcp::Handler;
using libtmux::mcp::StructuredValue;
using libtmux::mcp::ToolDefinition;
using libtmux::mcp::ToolOutput;
using libtmux::mcp::ToolRegistry;
using libtmux::mcp::ToolResult;
using libtmux::mcp::ToolSelection;
using libtmux::mcp::server::ProtocolEra;

[[nodiscard]] ToolRegistry all_tools() {
  auto built = default_tools();
  EXPECT_TRUE(built.has_value()) << built.error();
  return std::move(*built);
}

[[nodiscard]] json published_schemas() {
  const json listed =
      libtmux::mcp::server::tools_result(all_tools(), ProtocolEra::modern);
  json schemas = json::object();
  for (const auto& tool : listed.at("tools")) {
    schemas[tool.at("name").get<std::string>()] = tool.at("outputSchema");
  }
  return schemas;
}

[[nodiscard]] json published_output(const ToolOutput& answer) {
  return libtmux::mcp::server::tool_success(answer,
                                            ProtocolEra::legacy)["structuredContent"];
}

[[nodiscard]] libtmux::expected<ToolRegistry, std::string>
read_batch_with(Handler handler) {
  auto complete = default_tools(ToolSelection::all());
  if (!complete.has_value()) {
    return libtmux::unexpected(complete.error());
  }
  const ToolDefinition* const source_batch = complete->find("call_read_tools_batch");
  if (source_batch == nullptr) {
    return libtmux::unexpected(std::string{"read batch definition is missing"});
  }
  std::vector<ToolDefinition> definitions{*source_batch};
  for (const std::string& name : source_batch->authority.nested_tools) {
    ToolDefinition nested = *complete->find(name);
    nested.schema.input.clear();
    nested.authority.input_sinks.clear();
    nested.handler = handler;
    definitions.push_back(std::move(nested));
  }
  ToolSelection selection;
  selection.include = {"call_read_tools_batch"};
  return ToolRegistry::create(std::move(definitions), selection);
}

// Answers every command slower than the deadline it is given, so a wait bounded
// below that delay expires while still resolving its target.
class SlowBackend final : public libtmux::detail::Backend {
public:
  explicit SlowBackend(libtmux::ExecutionPolicy policy = {}) : Backend{{}, policy} {}
  mutable std::size_t command_count{};

  libtmux::expected<std::string, libtmux::CommandFailure>
  run(const libtmux::CommandRequest&, std::optional<std::chrono::milliseconds> timeout,
      std::optional<std::size_t>) const override {
    ++command_count;
    if (timeout.has_value()) {
      std::this_thread::sleep_for(*timeout);
    }
    return libtmux::unexpected(
        libtmux::CommandFailure{.kind = libtmux::FailureKind::timeout,
                                .delivery = libtmux::DeliveryStatus::replied,
                                .exit_code = -1,
                                .diagnostic = "scripted timeout"});
  }

  const std::vector<std::string>& connection() const noexcept override {
    return connection_;
  }

  libtmux::expected<libtmux::Version, libtmux::CommandFailure>
  version() const override {
    return libtmux::Version{.major = 3, .minor = 4};
  }

private:
  std::vector<std::string> connection_;
};

[[nodiscard]] ToolOutput wait_past_pane_lookup() {
  const Server server = libtmux::detail::server_over(std::make_shared<SlowBackend>());
  auto waited = all_tools().call(
      server, "wait_for_text",
      {{"target", "mcp"}, {"text", "never appears"}, {"timeout_ms", "1"}});
  EXPECT_TRUE(waited.has_value()) << waited.error().message;
  return waited.value_or(ToolOutput{});
}

TEST(McpProtocolSchema, InvalidLayoutIsRefusedBeforeTargetLookup) {
  auto backend = std::make_shared<SlowBackend>(
      libtmux::ExecutionPolicy{.timeout = std::chrono::milliseconds{1}});
  const auto server = libtmux::detail::server_over(backend);
  const auto tools = all_tools();
  for (const auto* layout : {"invalid-layout", "0000,80x24,0,0", "32d2,80x24,0,0{}"}) {
    const auto answer =
        tools.call(server, "select_layout", {{"windowId", "@999"}, {"layout", layout}});
    ASSERT_FALSE(answer.has_value());
    EXPECT_NE(answer.error().message.find("layout"), std::string::npos)
        << answer.error().message;
    EXPECT_EQ(backend->command_count, 0U);
  }
}

TEST(McpProtocolSchema, PreservesStructuredScalarTypes) {
  const libtmux::mcp::ToolOutput answer{
      .structured = {
          {"array", StructuredValue::Array{StructuredValue{}, true, 7, "mcp"}},
          {"boolean", true},
          {"integer", 7},
          {"null", StructuredValue{}},
          {"object", StructuredValue::Object{{"nested", "value"}}},
          {"string", "mcp"}}};
  const json result = libtmux::mcp::server::tool_success(
      answer, libtmux::mcp::server::ProtocolEra::legacy);
  const json& structured = result["structuredContent"];

  ASSERT_TRUE(structured["array"].is_array());
  EXPECT_TRUE(structured["array"][0].is_null());
  EXPECT_TRUE(structured["array"][1].is_boolean());
  EXPECT_TRUE(structured["array"][2].is_number_integer());
  EXPECT_TRUE(structured["array"][3].is_string());
  EXPECT_TRUE(structured["boolean"].is_boolean());
  EXPECT_TRUE(structured["integer"].is_number_integer());
  EXPECT_TRUE(structured["null"].is_null());
  EXPECT_TRUE(structured["object"].is_object());
  EXPECT_TRUE(structured["string"].is_string());
  EXPECT_EQ(structured["array"], json::array({nullptr, true, 7, "mcp"}));
  EXPECT_EQ(structured["object"], json({{"nested", "value"}}));
  EXPECT_EQ(result["content"][0]["text"], structured.dump());
}

TEST(McpProtocolSchema, DescribesPsmuxAsANamespaceWithoutAPosixAttachRoute) {
  const std::string instructions =
      libtmux::mcp::server::initialize_result("2025-06-18")["instructions"];
  EXPECT_NE(instructions.find("connection route"), std::string::npos);
  EXPECT_EQ(instructions.find("resolved path"), std::string::npos);
  EXPECT_EQ(instructions.find("attach command"), std::string::npos);

  const std::vector<std::pair<std::string, std::string>> cases{
      {"name:review", "psmux:-L:review"}, {"inherit", "psmux:default"}};
  for (const auto& [selector, endpoint] : cases) {
    SCOPED_TRACE(endpoint);
    const auto disclosure = libtmux::mcp::server::capability_disclosure(
        libtmux::ServerImplementation::psmux, selector, "operator-current", "existing",
        "unknown", endpoint);
    const json result = libtmux::mcp::server::capabilities_resource_result(
        all_tools(), ProtocolEra::legacy, disclosure);
    const json document = json::parse(result["contents"][0]["text"].get<std::string>());
    const json& connection = document["connection"];

    EXPECT_EQ(connection.value("namespaceSelector", ""), endpoint);
    EXPECT_TRUE(connection["resolvedSocketPath"].is_null());
    EXPECT_TRUE(connection["attachCommand"].is_null());
    EXPECT_EQ(document.dump().find("-S psmux:"), std::string::npos);
  }
}

TEST(McpProtocolSchema, PreservesThePosixSocketAndExactAttachRoute) {
  const auto disclosure = libtmux::mcp::server::capability_disclosure(
      libtmux::ServerImplementation::tmux, "name:review", "operator-current",
      "existing", "unknown", "/tmp/tmux-1000/review");
  const json result = libtmux::mcp::server::capabilities_resource_result(
      all_tools(), ProtocolEra::legacy, disclosure);
  const json document = json::parse(result["contents"][0]["text"].get<std::string>());

  EXPECT_EQ(document["connection"],
            json({{"socketSelector", "name:review"},
                  {"socketProvenance", "operator-current"},
                  {"resolvedSocketPath", "/tmp/tmux-1000/review"},
                  {"serverState", "existing"},
                  {"configurationProvenance", "unknown"},
                  {"attachCommand", "tmux -N -S '/tmp/tmux-1000/review' attach"}}));
}

TEST(McpProtocolSchema, CapsTheCompleteReadBatchResultAtOneMillionBytes) {
  const std::string payload(400U * 1024U, 'x');
  auto tools =
      read_batch_with([payload](const Server&, const Arguments&,
                                const libtmux::mcp::CallContext&) -> ToolResult {
        return ToolOutput{.structured = {{"payload", payload}}};
      });
  ASSERT_TRUE(tools.has_value()) << tools.error();
  Arguments arguments;
  arguments.read_calls = {{"list_sessions", {}}};
  auto server = Server::at_socket_name("mcp-complete-batch-cap");
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  const auto answer = tools->call(*server, "call_read_tools_batch", arguments);
  ASSERT_TRUE(answer.has_value()) << answer.error().message;

  for (const ProtocolEra era : {ProtocolEra::legacy, ProtocolEra::modern}) {
    const json wire = libtmux::mcp::server::tool_success(*answer, era);
    EXPECT_LE(wire.dump().size(), 1'000'000U);
    ASSERT_EQ(wire["structuredContent"]["results"].size(), 1U);
    EXPECT_TRUE(wire["structuredContent"]["truncated"].get<bool>());
    EXPECT_TRUE(wire["structuredContent"]["results"][0]["resultTruncated"].get<bool>());
  }
}

TEST(McpProtocolSchema, KeepsAllExecutedReadBatchRowsInsideTheWireCap) {
  const std::string error(70'000U, 'e');
  auto tools = read_batch_with([error](const Server&, const Arguments&,
                                       const libtmux::mcp::CallContext&) -> ToolResult {
    return libtmux::unexpected(libtmux::mcp::ToolError{false, error});
  });
  ASSERT_TRUE(tools.has_value()) << tools.error();
  Arguments arguments{{"onError", "continue"}};
  arguments.read_calls.assign(16U, {"list_sessions", {}});
  auto server = Server::at_socket_name("mcp-complete-batch-errors");
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  const auto answer = tools->call(*server, "call_read_tools_batch", arguments);
  ASSERT_TRUE(answer.has_value()) << answer.error().message;

  const json wire = libtmux::mcp::server::tool_success(*answer, ProtocolEra::modern);
  EXPECT_LE(wire.dump().size(), 1'000'000U);
  ASSERT_EQ(wire["structuredContent"]["results"].size(), 16U);
  EXPECT_EQ(wire["structuredContent"]["failed"], 16);
  for (std::size_t index = 0; index < 16U; ++index) {
    EXPECT_EQ(wire["structuredContent"]["results"][index]["index"], index);
  }
}

TEST(McpProtocolSchema, WaitOmitsThePaneIdItNeverResolved) {
  const json structured = published_output(wait_past_pane_lookup());
  EXPECT_TRUE(structured["timed_out"].get<bool>());
  EXPECT_FALSE(structured["matched"].get<bool>());
  EXPECT_EQ(structured["mode"], "pane-lookup");
  EXPECT_FALSE(structured.contains("pane_id"));
}

// Writes what a client actually receives — the published schema beside a real
// answer from every tool — for the validator in `tools/schema` to check.
TEST(McpProtocolSchemaTmux, EmitsEveryToolAnswerBesideItsPublishedSchema) {
  const char* const destination = std::getenv("LIBTMUX_MCP_OUTPUT_CORPUS");
  ASSERT_NE(destination, nullptr)
      << "LIBTMUX_MCP_OUTPUT_CORPUS names where to write the corpus";

  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  auto opened = Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(opened.has_value());
  const Server server = *opened;
  const std::string session{fixture->session_name()};
  const auto tools = all_tools();

  auto root = server.session(session);
  ASSERT_TRUE(root.has_value()) << root.error().diagnostic;
  auto windows = root->windows();
  ASSERT_TRUE(windows.has_value()) << windows.error().diagnostic;
  ASSERT_FALSE(windows->empty());
  const auto main_window = windows->front();
  auto panes = main_window.panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  ASSERT_FALSE(panes->empty());
  const auto primary = panes->front();
  auto secondary = main_window.split({.focus = false});
  ASSERT_TRUE(secondary.has_value()) << secondary.error().diagnostic;
  auto doomed_pane = main_window.split({.focus = false});
  ASSERT_TRUE(doomed_pane.has_value()) << doomed_pane.error().diagnostic;
  auto doomed_window =
      root->new_window({.name = "schema-doomed-window", .focus = false});
  ASSERT_TRUE(doomed_window.has_value()) << doomed_window.error().diagnostic;
  auto respawn_window =
      root->new_window({.name = "schema-respawn-window", .focus = false});
  ASSERT_TRUE(respawn_window.has_value()) << respawn_window.error().diagnostic;
  auto respawn_panes = respawn_window->panes();
  ASSERT_TRUE(respawn_panes.has_value()) << respawn_panes.error().diagnostic;
  ASSERT_FALSE(respawn_panes->empty());
  const auto respawn_pane = respawn_panes->front();
  auto doomed_session = server.new_session("schema-doomed-session");
  ASSERT_TRUE(doomed_session.has_value()) << doomed_session.error().diagnostic;

  const std::filesystem::path literal_directory =
      fixture->socket_path().parent_path() / "schema-#{pid}";
  std::error_code directory_error;
  ASSERT_TRUE(std::filesystem::create_directory(literal_directory, directory_error))
      << directory_error.message();

  Arguments batch;
  batch.read_calls.push_back({"list_sessions", {}});
  std::map<std::string, Arguments, std::less<>> calls;
  const auto add =
      [&calls](std::string name,
               std::initializer_list<std::pair<std::string_view, std::string_view>>
                   arguments) { calls.emplace(std::move(name), Arguments{arguments}); };
  add("list_sessions", {});
  add("list_windows", {{"session", root->id()}});
  add("list_panes", {});
  add("get_server_info", {});
  add("get_session_info", {{"session", root->id()}});
  add("get_window_info", {{"windowId", main_window.id()}});
  add("get_pane_info", {{"paneId", primary.id()}});
  add("capture_pane", {{"paneId", primary.id()}});
  add("capture_since", {{"paneId", primary.id()}, {"cursor", "0"}});
  add("snapshot_pane", {{"paneId", primary.id()}});
  add("search_panes", {{"pattern", "schema"}});
  add("find_pane_by_position",
      {{"row", "0"}, {"column", "0"}, {"windowId", main_window.id()}});
  add("wait_for_text",
      {{"target", primary.id()}, {"text", "never appears"}, {"timeout_ms", "1"}});
  Arguments variables{{"paneId", primary.id()}};
  variables.string_arrays["names"] = {"pane_id"};
  calls.emplace("get_tmux_variables", std::move(variables));
  add("show_option", {{"name", "history-limit"}, {"target", root->id()}});
  add("show_environment", {});
  add("show_hooks", {});
  calls.emplace("call_read_tools_batch", std::move(batch));
  add("rename_session", {{"session", root->id()}, {"name", "schema-#{pid}"}});
  add("rename_window", {{"windowId", main_window.id()}, {"name", "schema-#{pid}"}});
  add("select_window", {{"windowId", main_window.id()}});
  add("select_pane", {{"paneId", primary.id()}});
  add("select_layout", {{"windowId", main_window.id()}, {"layout", "tiled"}});
  add("resize_window",
      {{"windowId", main_window.id()}, {"width", "100"}, {"height", "40"}});
  add("resize_pane", {{"paneId", primary.id()}, {"width", "30"}});
  add("move_window", {{"windowId", main_window.id()}, {"index", "9"}});
  add("swap_pane", {{"sourcePaneId", primary.id()}, {"targetPaneId", secondary->id()}});
  add("set_pane_title", {{"paneId", primary.id()}, {"title", "schema-#{pid}"}});
  add("wait_for_channel", {{"channel", "schema-wait"}, {"timeoutMs", "1000"}});
  add("signal_channel", {{"channel", "schema-signal"}});
  add("set_mouse_enabled", {{"enabled", "false"}});
  add("set_history_limit", {{"session", root->id()}, {"limit", "2000"}});
  add("create_session", {{"name", "schema-created-#{pid}"},
                         {"windowName", "schema-first-#{pid}"},
                         {"startDirectory", literal_directory.string()}});
  add("create_window", {{"session", root->id()},
                        {"name", "schema-created-window-#{pid}"},
                        {"startDirectory", literal_directory.string()}});
  add("split_window",
      {{"paneId", primary.id()}, {"startDirectory", literal_directory.string()}});
  add("respawn_pane", {{"paneId", respawn_pane.id()},
                       {"force", "true"},
                       {"startDirectory", literal_directory.string()}});
  add("run_shell_command", {{"paneId", primary.id()},
                            {"command", "printf schema-conformance"},
                            {"timeoutMs", "5000"}});
  add("send_keys", {{"paneId", primary.id()}, {"keys", "Escape"}});
  Arguments key_batch;
  key_batch.send_key_operations.push_back(
      libtmux::mcp::FlatArguments{{"paneId", primary.id()}, {"keys", "Escape"}});
  calls.emplace("send_keys_batch", std::move(key_batch));
  add("paste_text", {{"paneId", primary.id()}, {"text", "schema-conformance"}});
  add("set_synchronize_panes", {{"windowId", main_window.id()}, {"enabled", "false"}});
  add("clear_pane_scrollback", {{"paneId", primary.id()}});
  add("kill_pane", {{"paneId", doomed_pane->id()}});
  add("kill_window", {{"windowId", doomed_window->id()}});
  add("kill_session", {{"session", doomed_session->id()}});
  const json schemas = published_schemas();

  std::ofstream out{destination, std::ios::trunc};
  ASSERT_TRUE(out.is_open()) << "cannot write " << destination;
  const auto emit = [&out, &schemas](const std::string& name, const json& document) {
    out << json{{"tool", name}, {"schema", schemas.at(name)}, {"document", document}}
               .dump()
        << '\n';
  };

  for (const ToolDefinition& tool : tools.tools()) {
    const auto call = calls.find(tool.name);
    ASSERT_NE(call, calls.end()) << tool.name << " has no schema-conformance call";
    std::optional<std::thread> signal;
    if (tool.name == "wait_for_channel") {
      signal.emplace([server] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        static_cast<void>(server.signal("schema-wait"));
      });
    }
    const auto result = tools.call(server, tool.name, call->second);
    if (signal.has_value()) {
      signal->join();
    }
    ASSERT_TRUE(result.has_value()) << tool.name << ": " << result.error().message;
    if (tool.name == "rename_session") {
      const auto renamed = server.session(root->id());
      ASSERT_TRUE(renamed.has_value()) << renamed.error().diagnostic;
      EXPECT_EQ(renamed->name(), "schema-#{pid}");
    } else if (tool.name == "rename_window") {
      const auto renamed = server.window(main_window.id());
      ASSERT_TRUE(renamed.has_value()) << renamed.error().diagnostic;
      EXPECT_EQ(renamed->name(), "schema-#{pid}");
    } else if (tool.name == "set_pane_title") {
      const auto titled = server.pane(primary.id());
      ASSERT_TRUE(titled.has_value()) << titled.error().diagnostic;
      EXPECT_EQ(titled->title(), "schema-#{pid}");
    } else if (tool.name == "create_session") {
      const auto& value = result->structured.at("session_id").value;
      const auto created = server.session(std::get<std::string>(value));
      ASSERT_TRUE(created.has_value()) << created.error().diagnostic;
      auto created_panes = created->panes();
      ASSERT_TRUE(created_panes.has_value()) << created_panes.error().diagnostic;
      ASSERT_FALSE(created_panes->empty());
      EXPECT_EQ(created_panes->front().path(), literal_directory.string());
    }
    emit(tool.name, published_output(*result));
  }
  // The one answer no live server produces on demand.
  emit("wait_for_text", published_output(wait_past_pane_lookup()));
  out.flush();
  ASSERT_TRUE(out.good()) << "cannot finish writing " << destination;
  EXPECT_TRUE(std::filesystem::remove(literal_directory, directory_error))
      << directory_error.message();
}

} // namespace
