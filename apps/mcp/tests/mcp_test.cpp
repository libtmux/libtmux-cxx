#include "libtmux_consumers/mcp.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "backend.hpp"
#include "libtmux/format.hpp"
#include "libtmux/server.hpp"
#include "libtmux/testing/scoped_server.hpp"

namespace {

using libtmux::Server;
using libtmux::mcp::Arguments;
using libtmux::mcp::default_tools;
using libtmux::mcp::OutputShape;
using libtmux::mcp::Parameter;
using libtmux::mcp::StructuredValue;
using libtmux::mcp::Tool;
using libtmux::mcp::ToolAnnotations;
using libtmux::mcp::ToolOutput;
using libtmux::mcp::ToolResult;
using libtmux::mcp::ToolSet;

class DeadlineBackend final : public libtmux::detail::Backend {
public:
  explicit DeadlineBackend(std::chrono::milliseconds delay) : delay_{delay} {}

  libtmux::expected<std::string, libtmux::CommandFailure>
  run(const std::vector<std::string>& command,
      std::optional<std::chrono::milliseconds> timeout,
      std::optional<std::size_t>) const override {
    timeouts_.push_back(timeout);
    if (timeout.has_value() && *timeout <= delay_) {
      std::this_thread::sleep_for(*timeout);
      return libtmux::unexpected(
          libtmux::CommandFailure{.kind = libtmux::FailureKind::timeout,
                                  .dispatched = true,
                                  .exit_code = -1,
                                  .diagnostic = "scripted timeout"});
    }
    std::this_thread::sleep_for(delay_);
    if (command.empty()) {
      return libtmux::unexpected(
          libtmux::CommandFailure{.kind = libtmux::FailureKind::validation,
                                  .dispatched = false,
                                  .exit_code = 0,
                                  .diagnostic = "empty scripted command"});
    }
    if (command.front() == "capture-pane") {
      return "visible text without the marker\n";
    }
    if (command.front() == "display-message" &&
        command.back().find("pane_id") != std::string::npos) {
      std::string row{"%1"};
      row += libtmux::kFormatSeparator;
      row += "mcp";
      row += libtmux::kFormatSeparator;
      row += '\n';
      return row;
    }
    if (command.front() == "display-message" && command.back() == "#{socket_path}") {
      return "\n";
    }
    return libtmux::unexpected(
        libtmux::CommandFailure{.kind = libtmux::FailureKind::validation,
                                .dispatched = false,
                                .exit_code = 0,
                                .diagnostic = "unexpected scripted command"});
  }

  const std::vector<std::string>& connection() const noexcept override {
    return connection_;
  }

  libtmux::expected<libtmux::Version, libtmux::CommandFailure>
  version() const override {
    return libtmux::Version{.major = 3, .minor = 4};
  }

  [[nodiscard]] const std::vector<std::optional<std::chrono::milliseconds>>&
  timeouts() const noexcept {
    return timeouts_;
  }

private:
  std::chrono::milliseconds delay_;
  std::vector<std::string> connection_;
  mutable std::vector<std::optional<std::chrono::milliseconds>> timeouts_;
};

Server connect(const libtmux::test::ScopedTmuxServer& fixture) {
  auto server = Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value());
  return server.value();
}

const std::string& string_field(const ToolOutput& output, std::string_view name) {
  return std::get<std::string>(output.structured.at(std::string{name}).value);
}

TEST(McpToolsTmux, ListsTheSessionsOfARealServer) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto result = default_tools().call(connect(*fixture), "list_sessions", {});
  ASSERT_TRUE(result.has_value()) << result.error().message;
  const auto& sessions =
      std::get<StructuredValue::Array>(result->structured.at("sessions").value);
  ASSERT_EQ(sessions.size(), 1U);
  const auto& session = std::get<StructuredValue::Object>(sessions.front().value);
  EXPECT_EQ(std::get<std::string>(session.at("name").value), fixture->session_name());
}

TEST(McpToolsTmux, SeparatesACallerMistakeFromATmuxRefusal) {
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

TEST(McpToolsTmux, CapturesAPaneThroughTheLibrary) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const auto captured =
      default_tools().call(server, "capture_pane",
                           Arguments{{"target", std::string{fixture->session_name()}}});
  ASSERT_TRUE(captured.has_value()) << captured.error().message;
}

TEST(McpToolsTmux, CreatesAWindowAndTypesIntoItsPane) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const auto tools = default_tools();
  const std::string session{fixture->session_name()};

  const auto created = tools.call(
      server, "new_window", Arguments{{"session", session}, {"name", "from-mcp"}});
  ASSERT_TRUE(created.has_value()) << created.error().message;
  const std::string window_id = string_field(*created, "window_id");
  EXPECT_EQ(window_id.front(), '@');

  const auto typed = tools.call(server, "send_text",
                                Arguments{{"target", window_id}, {"text", "marker"}});
  ASSERT_TRUE(typed.has_value()) << typed.error().message;

  const auto listed = tools.call(server, "list_panes", {});
  ASSERT_TRUE(listed.has_value()) << listed.error().message;
  const auto& panes =
      std::get<StructuredValue::Array>(listed->structured.at("panes").value);
  EXPECT_TRUE(std::ranges::any_of(panes, [&window_id](const StructuredValue& value) {
    const auto& pane = std::get<StructuredValue::Object>(value.value);
    return std::get<std::string>(pane.at("window_id").value) == window_id;
  }));
}

TEST(McpTools, EveryToolDeclaresANameAndDescription) {
  // The set must be named: tools() returns a reference into it, and a
  // range-for over the temporary would outlive what it borrows.
  const auto tools = default_tools();
  for (const auto& tool : tools.tools()) {
    EXPECT_FALSE(tool.name.empty());
    EXPECT_FALSE(tool.description.empty());
  }
  EXPECT_EQ(tools.tools().size(), 12U);
}

TEST(McpTools, CountsUtf8CodePointsLikeThePublishedSchema) {
  ToolSet tools;
  tools.add(Tool{.name = "unicode",
                 .title = "Unicode",
                 .description = "Validate a bounded Unicode string.",
                 .parameters = {{.name = "value",
                                 .description = "At most two characters.",
                                 .maximum_length = 2U}},
                 .output = OutputShape::pane_text,
                 .annotations = ToolAnnotations{},
                 .handle = [](const Server&, const Arguments&,
                              const libtmux::mcp::CallContext&) -> ToolResult {
                   return ToolOutput{.structured = {}};
                 }});
  auto server = Server::at_socket_name("mcp-unicode-validation");
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;

  const std::string face{"\xF0\x9F\x98\x80"};
  const auto accepted = tools.call(*server, "unicode", {{"value", face + face}});
  ASSERT_TRUE(accepted.has_value()) << accepted.error().message;

  const auto too_long = tools.call(*server, "unicode", {{"value", face + face + face}});
  ASSERT_FALSE(too_long.has_value());
  EXPECT_TRUE(too_long.error().caller_error);
  EXPECT_NE(too_long.error().message.find("2 characters"), std::string::npos);

  const std::vector<std::string> invalid{
      {static_cast<char>(0xC0), static_cast<char>(0x80)},
      {static_cast<char>(0xED), static_cast<char>(0xA0), static_cast<char>(0x80)},
      {static_cast<char>(0xF4), static_cast<char>(0x90), static_cast<char>(0x80),
       static_cast<char>(0x80)},
      {static_cast<char>(0xE2), static_cast<char>(0x82)},
  };
  for (const std::string& value : invalid) {
    const auto malformed = tools.call(*server, "unicode", {{"value", value}});
    ASSERT_FALSE(malformed.has_value());
    EXPECT_TRUE(malformed.error().caller_error);
    EXPECT_NE(malformed.error().message.find("valid UTF-8"), std::string::npos);
  }
}

TEST(McpTools, AppliesOneDeadlineToWaitStartupAndPolling) {
  using namespace std::chrono_literals;
  auto backend = std::make_shared<DeadlineBackend>(6ms);
  const Server server = libtmux::detail::server_over(backend);
  const auto started = std::chrono::steady_clock::now();
  const auto waited = default_tools().call(
      server, "wait_for_text",
      {{"target", "mcp"}, {"text", "never appears"}, {"timeout_ms", "30"}});
  const auto elapsed = std::chrono::steady_clock::now() - started;

  ASSERT_TRUE(waited.has_value()) << waited.error().message;
  EXPECT_TRUE(std::get<bool>(waited->structured.at("timed_out").value));
  EXPECT_FALSE(std::get<bool>(waited->structured.at("matched").value));
  EXPECT_LT(elapsed, 100ms);

  // A shared deadline shows in every later command asking for less, not in how
  // many commands fit: a loaded machine spends the same budget on fewer.
  const auto& timeouts = backend->timeouts();
  ASSERT_FALSE(timeouts.empty());
  for (std::size_t index = 0; index < timeouts.size(); ++index) {
    ASSERT_TRUE(timeouts[index].has_value());
    EXPECT_GT(*timeouts[index], 0ms);
    EXPECT_LE(*timeouts[index], 30ms);
    if (index > 0) {
      EXPECT_LE(*timeouts[index], *timeouts[index - 1]);
    }
  }
  if (timeouts.size() >= 2U) {
    EXPECT_LT(*timeouts.back(), *timeouts.front());
  }
}

TEST(McpTools, ObservesCancellationBetweenBoundedWaitCommands) {
  using namespace std::chrono_literals;
  auto backend = std::make_shared<DeadlineBackend>(6ms);
  const Server server = libtmux::detail::server_over(backend);
  std::atomic_bool cancelled{false};
  std::thread cancel{[&cancelled] {
    std::this_thread::sleep_for(10ms);
    cancelled.store(true);
  }};
  const auto started = std::chrono::steady_clock::now();
  const auto waited = default_tools().call(
      server, "wait_for_text",
      {{"target", "mcp"}, {"text", "never appears"}, {"timeout_ms", "1000"}},
      libtmux::mcp::CallContext{
          .is_cancelled = [&cancelled] { return cancelled.load(); }});
  const auto elapsed = std::chrono::steady_clock::now() - started;
  cancel.join();

  ASSERT_FALSE(waited.has_value());
  EXPECT_FALSE(waited.error().caller_error);
  EXPECT_EQ(waited.error().message, "request cancelled");
  EXPECT_LT(elapsed, 100ms);
}

TEST(McpToolsTmux, ReportsAPaneThatDisappearsDuringSearch) {
  auto started = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(started.has_value()) << started.error();
  auto fixture = std::make_unique<libtmux::test::ScopedTmuxServer>(*std::move(started));
  const std::string path = fixture->socket_path().string();
  bool stopped = false;
  auto server = Server::at_socket_path(
      path,
      [&fixture, &stopped](std::string_view command, const libtmux::CommandFailure*) {
        if (!stopped && command.find("list-panes") != std::string_view::npos) {
          stopped = true;
          fixture.reset();
        }
      });
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;

  const auto searched =
      default_tools().call(*server, "search_panes", Arguments{{"text", "anything"}});
  ASSERT_FALSE(searched.has_value());
  EXPECT_FALSE(searched.error().caller_error);
  EXPECT_FALSE(searched.error().message.empty());
}

} // namespace
