#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
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
using libtmux::mcp::StructuredValue;
using libtmux::mcp::Tool;
using libtmux::mcp::ToolOutput;
using libtmux::mcp::server::ProtocolEra;

[[nodiscard]] json published_schemas() {
  const json listed =
      libtmux::mcp::server::tools_result(default_tools(), ProtocolEra::modern);
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

// Answers every command slower than the deadline it is given, so a wait bounded
// below that delay expires while still resolving its target.
class SlowBackend final : public libtmux::detail::Backend {
public:
  libtmux::expected<std::string, libtmux::CommandFailure>
  run(const libtmux::CommandRequest&, std::optional<std::chrono::milliseconds> timeout,
      std::optional<std::size_t>) const override {
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
  auto waited = default_tools().call(
      server, "wait_for_text",
      {{"target", "mcp"}, {"text", "never appears"}, {"timeout_ms", "1"}});
  EXPECT_TRUE(waited.has_value()) << waited.error().message;
  return waited.value_or(ToolOutput{});
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
  const auto tools = default_tools();

  const std::map<std::string, Arguments, std::less<>> calls{
      {"capture_pane", {{"target", session}}},
      {"create_session", {{"name", "schema-conformance"}}},
      {"inspect_tmux", {}},
      {"list_panes", {}},
      {"list_session_panes", {{"session", session}}},
      {"list_sessions", {}},
      {"list_windows", {{"session", session}}},
      {"new_window", {{"session", session}, {"name", "schema-conformance"}}},
      {"search_panes", {{"text", "conformance"}}},
      {"send_keys", {{"target", session}, {"keys", "Enter"}}},
      {"send_text", {{"target", session}, {"text", "conformance\n"}}},
      {"wait_for_text",
       {{"target", session}, {"text", "never appears"}, {"timeout_ms", "50"}}},
  };
  const json schemas = published_schemas();

  std::ofstream out{destination, std::ios::trunc};
  ASSERT_TRUE(out.is_open()) << "cannot write " << destination;
  const auto emit = [&out, &schemas](const std::string& name, const json& document) {
    out << json{{"tool", name}, {"schema", schemas.at(name)}, {"document", document}}
               .dump()
        << '\n';
  };

  for (const Tool& tool : tools.tools()) {
    const auto call = calls.find(tool.name);
    ASSERT_NE(call, calls.end()) << tool.name << " has no schema-conformance call";
    const auto result = tools.call(server, tool.name, call->second);
    ASSERT_TRUE(result.has_value()) << tool.name << ": " << result.error().message;
    emit(tool.name, published_output(*result));
  }
  // The one answer no live server produces on demand.
  emit("wait_for_text", published_output(wait_past_pane_lookup()));
  out.flush();
  ASSERT_TRUE(out.good()) << "cannot finish writing " << destination;
}

} // namespace
