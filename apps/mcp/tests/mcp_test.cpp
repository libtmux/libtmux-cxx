#include "libtmux_consumers/mcp.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "backend.hpp"
#include "libtmux/format.hpp"
#include "libtmux/server.hpp"
#include "libtmux/testing/scoped_server.hpp"
#include "tool_support.hpp"

namespace {

using libtmux::Server;
using libtmux::mcp::Arguments;
using libtmux::mcp::default_tools;
using libtmux::mcp::Effect;
using libtmux::mcp::FlatArguments;
using libtmux::mcp::InputControl;
using libtmux::mcp::InputSink;
using libtmux::mcp::NestedAuthority;
using libtmux::mcp::OutputClass;
using libtmux::mcp::OutputShape;
using libtmux::mcp::ProcessReach;
using libtmux::mcp::Sink;
using libtmux::mcp::StructuredValue;
using libtmux::mcp::ToolDefinition;
using libtmux::mcp::ToolOutput;
using libtmux::mcp::ToolRegistry;
using libtmux::mcp::ToolResult;
using libtmux::mcp::ToolSelection;
using libtmux::mcp::Toolset;

class DeadlineBackend final : public libtmux::detail::Backend {
public:
  explicit DeadlineBackend(std::chrono::milliseconds delay,
                           std::string capture = "visible text without the marker\n")
      : delay_{delay}, capture_{std::move(capture)} {}

  libtmux::expected<std::string, libtmux::CommandFailure>
  run(const libtmux::CommandRequest& command,
      std::optional<std::chrono::milliseconds> timeout,
      std::optional<std::size_t>) const override {
    timeouts_.push_back(timeout);
    if (timeout.has_value() && *timeout <= delay_) {
      std::this_thread::sleep_for(*timeout);
      return libtmux::unexpected(
          libtmux::CommandFailure{.kind = libtmux::FailureKind::timeout,
                                  .delivery = libtmux::DeliveryStatus::replied,
                                  .exit_code = -1,
                                  .diagnostic = "scripted timeout"});
    }
    std::this_thread::sleep_for(delay_);
    if (command.empty()) {
      return libtmux::unexpected(
          libtmux::CommandFailure{.kind = libtmux::FailureKind::validation,
                                  .delivery = libtmux::DeliveryStatus::not_started,
                                  .exit_code = 0,
                                  .diagnostic = "empty scripted command"});
    }
    const std::vector<std::string> argv = command.argv();
    if (argv.front() == "capture-pane") {
      return capture_;
    }
    if (argv.front() == "display-message" &&
        argv.back().find("pane_id") != std::string::npos) {
      std::string row{"%1"};
      row += libtmux::kFormatSeparator;
      row += "mcp";
      row += libtmux::kFormatSeparator;
      row += '\n';
      return row;
    }
    if (argv.front() == "display-message" && argv.back() == "#{socket_path}") {
      return "\n";
    }
    return libtmux::unexpected(
        libtmux::CommandFailure{.kind = libtmux::FailureKind::validation,
                                .delivery = libtmux::DeliveryStatus::not_started,
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
  std::string capture_;
  std::vector<std::string> connection_;
  mutable std::vector<std::optional<std::chrono::milliseconds>> timeouts_;
};

Server connect(const libtmux::test::ScopedTmuxServer& fixture) {
  auto server = Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value());
  return server.value();
}

class SelectedSocketReplacement final {
public:
  SelectedSocketReplacement(std::filesystem::path selected,
                            std::filesystem::path retained,
                            const std::filesystem::path& replacement)
      : selected_{std::move(selected)}, retained_{std::move(retained)} {
    removed_ = std::filesystem::remove(selected_, error_);
    if (error_ || !removed_) {
      if (!error_) {
        error_ = std::make_error_code(std::errc::no_such_file_or_directory);
      }
      return;
    }
    std::filesystem::create_hard_link(replacement, selected_, error_);
  }

  ~SelectedSocketReplacement() { static_cast<void>(restore()); }

  SelectedSocketReplacement(const SelectedSocketReplacement&) = delete;
  SelectedSocketReplacement& operator=(const SelectedSocketReplacement&) = delete;

  [[nodiscard]] const std::error_code& error() const noexcept { return error_; }

  [[nodiscard]] std::error_code restore() noexcept {
    if (!removed_) {
      return {};
    }
    std::error_code ignored;
    static_cast<void>(std::filesystem::remove(selected_, ignored));
    std::error_code restored;
    std::filesystem::create_hard_link(retained_, selected_, restored);
    if (!restored) {
      removed_ = false;
    }
    return restored;
  }

private:
  std::filesystem::path selected_;
  std::filesystem::path retained_;
  std::error_code error_;
  bool removed_{};
};

const std::string& string_field(const ToolOutput& output, std::string_view name) {
  return std::get<std::string>(output.structured.at(std::string{name}).value);
}

ToolRegistry all_tools() {
  auto built = default_tools();
  EXPECT_TRUE(built.has_value()) << built.error();
  return std::move(*built);
}

std::string pane_input_row(std::string_view pane_id, std::string_view window_id,
                           std::string_view synchronized, std::string_view mode,
                           std::string_view dead, std::string_view command) {
  std::string row;
  for (const std::string_view value :
       {pane_id, window_id, synchronized, mode, dead, command}) {
    row += value;
    row += libtmux::kFormatSeparator;
  }
  row += '\n';
  return row;
}

std::size_t occurrences(std::string_view text, std::string_view needle) {
  std::size_t count = 0U;
  std::size_t offset = 0U;
  while ((offset = text.find(needle, offset)) != std::string_view::npos) {
    ++count;
    offset += needle.size();
  }
  return count;
}

class ExecutableTree final {
public:
  ExecutableTree() {
    static std::atomic_uint64_t sequence{0U};
    root_ = std::filesystem::temp_directory_path() /
            ("libtmux-mcp-executable-" + std::to_string(++sequence));
    std::filesystem::create_directories(root_);
  }

  ~ExecutableTree() {
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }

  ExecutableTree(const ExecutableTree&) = delete;
  ExecutableTree& operator=(const ExecutableTree&) = delete;

  [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

  [[nodiscard]] std::filesystem::path executable(std::string_view directory,
                                                 bool executable = true) const {
    const std::filesystem::path parent = root_ / directory;
    std::filesystem::create_directories(parent);
    const std::filesystem::path path = parent / "tmux";
    std::ofstream{path} << "fixture\n";
    std::filesystem::permissions(path,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write |
                                     (executable ? std::filesystem::perms::owner_exec
                                                 : std::filesystem::perms::none),
                                 std::filesystem::perm_options::replace);
    return path;
  }

private:
  std::filesystem::path root_;
};

TEST(McpToolsTmux, ListsTheSessionsOfARealServer) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto result = all_tools().call(connect(*fixture), "list_sessions", {});
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
  const auto tools = all_tools();

  const auto missing = tools.call(server, "capture_pane", {});
  ASSERT_FALSE(missing.has_value());
  EXPECT_TRUE(missing.error().caller_error);

  const auto unknown = tools.call(server, "no_such_tool", {});
  ASSERT_FALSE(unknown.has_value());
  EXPECT_TRUE(unknown.error().caller_error);

  const auto refused =
      tools.call(server, "capture_pane", Arguments{{"paneId", "%999"}});
  ASSERT_FALSE(refused.has_value());
  EXPECT_FALSE(refused.error().caller_error);
}

TEST(McpToolsTmux, RunsShellFramingThroughThePinnedServerEndpoint) {
  auto original = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(original.has_value()) << original.error();
  auto replacement = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(replacement.has_value()) << replacement.error();
  const Server server = connect(*original);
  const Server other = connect(*replacement);

  auto panes = server.panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  ASSERT_FALSE(panes->empty());
  auto sessions = server.sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  ASSERT_FALSE(sessions->empty());
  const auto attach = sessions->front().attach_command();
  ASSERT_TRUE(attach.has_value()) << attach.error().diagnostic;
  ASSERT_GE(attach->argv().size(), 3U);
  ASSERT_EQ(attach->argv()[1], "-S");
  const std::filesystem::path retained = attach->argv()[2];
  const std::filesystem::path selected = original->socket_path();
  std::error_code compared;
  ASSERT_TRUE(std::filesystem::equivalent(retained, selected, compared));
  ASSERT_FALSE(compared) << compared.message();

  ASSERT_TRUE(other.run({"set-option", "-g", "@mcp-frame-route", "clean"}).has_value());
  ASSERT_TRUE(other
                  .run({"set-hook", "-g", "after-display-message",
                        "set-option -g @mcp-frame-route replacement"})
                  .has_value());

  {
    SelectedSocketReplacement replaced{selected, retained, replacement->socket_path()};
    ASSERT_FALSE(replaced.error()) << replaced.error().message();
    EXPECT_EQ(server.socket_path(), selected.string());

    const auto answer = all_tools().call(server, "run_shell_command",
                                         {{"paneId", panes->front().id()},
                                          {"command", "printf pinned-endpoint-output"},
                                          {"timeoutMs", "2000"}});
    ASSERT_TRUE(answer.has_value()) << answer.error().message;
    EXPECT_EQ(string_field(*answer, "text"), "pinned-endpoint-output");

    const auto replacement_route =
        other.run({"show-options", "-gv", "@mcp-frame-route"});
    ASSERT_TRUE(replacement_route.has_value()) << replacement_route.error().diagnostic;
    EXPECT_EQ(*replacement_route, "clean\n");
    const auto replacement_capture = other.panes();
    ASSERT_TRUE(replacement_capture.has_value())
        << replacement_capture.error().diagnostic;
    ASSERT_FALSE(replacement_capture->empty());
    const auto text = replacement_capture->front().capture();
    ASSERT_TRUE(text.has_value()) << text.error().diagnostic;
    EXPECT_EQ(text->find("pinned-endpoint-output"), std::string::npos);

    const std::error_code restored = replaced.restore();
    ASSERT_FALSE(restored) << restored.message();
  }
  compared.clear();
  EXPECT_TRUE(std::filesystem::equivalent(retained, selected, compared));
  EXPECT_FALSE(compared) << compared.message();
}

TEST(McpToolsTmux, CapturesAPaneThroughTheLibrary) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const auto captured =
      all_tools().call(server, "capture_pane",
                       Arguments{{"paneId", std::string{fixture->session_name()}}});
  ASSERT_TRUE(captured.has_value()) << captured.error().message;
}

TEST(McpTools, PaneInputSnapshotRejectsIncompleteAndBlankRecordFraming) {
  using libtmux::mcp::detail::PaneInputScope;
  const std::string row = pane_input_row("%7", "@3", "0", "0", "0", "sh");
  const std::string separator{libtmux::kFormatSeparator};
  const std::string short_row = "%7" + separator + "@3" + separator + "0" + separator +
                                "0" + separator + "0" + separator + "\n";
  std::string long_row = row;
  long_row.insert(long_row.size() - 1U, "extra" + separator);
  const std::array<std::string, 7> malformed{"",
                                             row.substr(0, row.size() - 1U),
                                             "\n",
                                             "\n" + row,
                                             row + "\n",
                                             row + "\n" + row,
                                             row + row.substr(0, row.size() - 1U)};
  for (const std::string& recording : malformed) {
    EXPECT_FALSE(libtmux::mcp::detail::parse_pane_input_snapshot(
                     "%7", recording, PaneInputScope::effective_cohort)
                     .has_value())
        << recording;
  }
  EXPECT_FALSE(libtmux::mcp::detail::parse_pane_input_snapshot(
                   "%7", short_row, PaneInputScope::effective_cohort)
                   .has_value());
  EXPECT_FALSE(libtmux::mcp::detail::parse_pane_input_snapshot(
                   "%7", long_row, PaneInputScope::effective_cohort)
                   .has_value());
  const auto refused = libtmux::mcp::detail::parse_pane_input_snapshot(
      "%7", "", PaneInputScope::effective_cohort);
  ASSERT_FALSE(refused.has_value());
  EXPECT_NE(refused.error().message.find("%7"), std::string::npos);
  EXPECT_TRUE(libtmux::mcp::detail::parse_pane_input_snapshot(
                  "%7", row, PaneInputScope::effective_cohort)
                  .has_value());
}

TEST(McpTools, PaneInputSnapshotRequiresCanonicalRowsAndExactState) {
  using libtmux::mcp::detail::PaneInputScope;
  const auto parse = [](std::string recording) {
    return libtmux::mcp::detail::parse_pane_input_snapshot(
        "%7", std::move(recording), PaneInputScope::effective_cohort);
  };
  const std::array<std::string_view, 9> invalid_panes{
      "", "%", "%00", "%07", "%+7", "%-7", "7", "%7x", "%18446744073709551616"};
  for (const std::string_view pane_id : invalid_panes) {
    EXPECT_FALSE(parse(pane_input_row(pane_id, "@3", "0", "0", "0", "sh")).has_value())
        << pane_id;
    EXPECT_FALSE(libtmux::mcp::detail::parse_pane_input_snapshot(
                     "%7",
                     pane_input_row("%7", "@3", "1", "0", "0", "sh") +
                         pane_input_row(pane_id, "@3", "1", "0", "0", "cat"),
                     PaneInputScope::effective_cohort)
                     .has_value())
        << pane_id;
    EXPECT_FALSE(libtmux::mcp::detail::parse_pane_input_snapshot(
                     "%7",
                     pane_input_row("%7", "@3", "0", "0", "0", "sh") +
                         pane_input_row(pane_id, "@3", "1", "0", "0", "cat"),
                     PaneInputScope::effective_cohort)
                     .has_value())
        << pane_id;
    EXPECT_FALSE(libtmux::mcp::detail::parse_pane_input_snapshot(
                     pane_id, pane_input_row("%7", "@3", "0", "0", "0", "sh"),
                     PaneInputScope::effective_cohort)
                     .has_value())
        << pane_id;
  }
  const std::array<std::string_view, 9> invalid_windows{
      "", "@", "@00", "@03", "@+3", "@-3", "3", "@3x", "@18446744073709551616"};
  for (const std::string_view window_id : invalid_windows) {
    EXPECT_FALSE(
        parse(pane_input_row("%7", window_id, "0", "0", "0", "sh")).has_value())
        << window_id;
  }
  for (const std::string_view state : {"", "00", "2", "on", "-1"}) {
    EXPECT_FALSE(parse(pane_input_row("%7", "@3", state, "0", "0", "sh")).has_value())
        << state;
    EXPECT_FALSE(parse(pane_input_row("%7", "@3", "0", "0", state, "sh")).has_value())
        << state;
  }
  for (const std::string_view mode :
       {"", "00", "+0", "-1", "text", "18446744073709551616"}) {
    EXPECT_FALSE(parse(pane_input_row("%7", "@3", "0", mode, "0", "sh")).has_value())
        << mode;
  }
  EXPECT_FALSE(parse(pane_input_row("%7", "@3", "0", "0", "0", "")).has_value());
  EXPECT_TRUE(parse(pane_input_row("%7", "@0", "0", "0", "0", "sh")).has_value());
  EXPECT_TRUE(libtmux::mcp::detail::parse_pane_input_snapshot(
                  "%0", pane_input_row("%0", "@3", "0", "0", "0", "sh"),
                  PaneInputScope::effective_cohort)
                  .has_value());
}

TEST(McpTools, PaneInputSnapshotValidatesAllRowsBeforeSelectingMembership) {
  using libtmux::mcp::detail::PaneInputScope;
  const std::string source = pane_input_row("%7", "@3", "0", "0", "0", "sh");
  const std::string ignored = pane_input_row("%8", "@3", "1", "2", "1", "cat");
  const auto source_only = libtmux::mcp::detail::parse_pane_input_snapshot(
      "%7", ignored + source, PaneInputScope::effective_cohort);
  ASSERT_TRUE(source_only.has_value()) << source_only.error().message;
  EXPECT_EQ(source_only->configured_pane_ids, std::vector<std::string>{"%7"});
  EXPECT_EQ(source_only->foreground_command, "sh");

  EXPECT_FALSE(libtmux::mcp::detail::parse_pane_input_snapshot(
                   "%7", source + pane_input_row("%8", "@3", "1", "00", "0", "cat"),
                   PaneInputScope::effective_cohort)
                   .has_value());
  EXPECT_FALSE(libtmux::mcp::detail::parse_pane_input_snapshot(
                   "%7", source + pane_input_row("%8", "@3", "1", "0", "0", ""),
                   PaneInputScope::effective_cohort)
                   .has_value());
  EXPECT_FALSE(libtmux::mcp::detail::parse_pane_input_snapshot(
                   "%7", source + source, PaneInputScope::effective_cohort)
                   .has_value());
  const std::string peer = pane_input_row("%8", "@3", "0", "0", "0", "sh");
  EXPECT_FALSE(libtmux::mcp::detail::parse_pane_input_snapshot(
                   "%7", source + peer + peer, PaneInputScope::effective_cohort)
                   .has_value());
  EXPECT_FALSE(libtmux::mcp::detail::parse_pane_input_snapshot(
                   "%7", pane_input_row("%8", "@3", "0", "0", "0", "sh"),
                   PaneInputScope::effective_cohort)
                   .has_value());
  EXPECT_FALSE(libtmux::mcp::detail::parse_pane_input_snapshot(
                   "%7", source + pane_input_row("%8", "@4", "0", "0", "0", "sh"),
                   PaneInputScope::effective_cohort)
                   .has_value());
}

TEST(McpTools, PaneInputSnapshotAppliesScopeAfterCohortValidation) {
  using libtmux::mcp::detail::PaneInputScope;
  const std::string source_on = pane_input_row("%7", "@3", "1", "0", "0", "bash");
  const std::string peer_on = pane_input_row("%12", "@3", "1", "0", "0", "cat");
  const std::string peer_off = pane_input_row("%8", "@3", "0", "2", "1", "sleep");

  const auto cohort = libtmux::mcp::detail::parse_pane_input_snapshot(
      "%7", peer_on + peer_off + source_on, PaneInputScope::effective_cohort);
  ASSERT_TRUE(cohort.has_value()) << cohort.error().message;
  EXPECT_EQ(cohort->configured_pane_ids, (std::vector<std::string>{"%12", "%7"}));

  const auto target = libtmux::mcp::detail::parse_pane_input_snapshot(
      "%7", peer_on + peer_off + source_on, PaneInputScope::target_only);
  ASSERT_TRUE(target.has_value()) << target.error().message;
  EXPECT_EQ(target->configured_pane_ids, std::vector<std::string>{"%7"});

  EXPECT_FALSE(libtmux::mcp::detail::parse_pane_input_snapshot(
                   "%7", peer_on + source_on, PaneInputScope::singular_posix_shell)
                   .has_value());
  EXPECT_FALSE(libtmux::mcp::detail::parse_pane_input_snapshot(
                   "%7", pane_input_row("%7", "@3", "0", "2", "0", "sh"),
                   PaneInputScope::target_only)
                   .has_value());
  EXPECT_FALSE(libtmux::mcp::detail::parse_pane_input_snapshot(
                   "%7", pane_input_row("%7", "@3", "0", "0", "1", "sh"),
                   PaneInputScope::target_only)
                   .has_value());
}

TEST(McpTools, PaneInputSnapshotAllowsOnlySupportedSingularPosixShells) {
  using libtmux::mcp::detail::PaneInputScope;
  for (const std::string_view command :
       {"sh", "ash", "/bin/bash", "dash", "ksh", "mksh", "pdksh", "-zsh"}) {
    EXPECT_TRUE(libtmux::mcp::detail::parse_pane_input_snapshot(
                    "%7", pane_input_row("%7", "@3", "0", "0", "0", command),
                    PaneInputScope::singular_posix_shell)
                    .has_value())
        << command;
  }
  for (const std::string_view command :
       {"cat", "sleep", "vim", "fish", "mybash", "bash-wrapper", "--bash", "/"}) {
    EXPECT_FALSE(libtmux::mcp::detail::parse_pane_input_snapshot(
                     "%7", pane_input_row("%7", "@3", "0", "0", "0", command),
                     PaneInputScope::singular_posix_shell)
                     .has_value())
        << command;
  }
}

TEST(McpTools, ShellCommandPayloadUsesAnIsolatedExactEndpointFrame) {
  const std::string nonce(32U, 'a');
  const auto payload = libtmux::mcp::detail::shell_command_payload(
      "true", "/opt/tmux", "/tmp/mcp.sock", [nonce] { return nonce; });
  ASSERT_TRUE(payload.has_value()) << payload.error().message;
  EXPECT_EQ(payload->marker, "__LIBTMUX_MCP_DONE_" + nonce + "__");
  EXPECT_EQ(payload->text.find(payload->marker), std::string::npos);
  EXPECT_EQ(payload->text.find("printf"), std::string::npos);
  EXPECT_EQ(payload->text.find("echo"), std::string::npos);
  EXPECT_EQ(payload->text.find("__libtmux_mcp_status"), std::string::npos);
  EXPECT_NE(payload->text.find("'/opt/tmux' -N -S '/tmp/mcp.sock' display-message -p"),
            std::string::npos);
  EXPECT_EQ(occurrences(payload->text, "\\eval "), 4U);
  EXPECT_EQ(occurrences(payload->text, "\\trap "), 4U);
  EXPECT_EQ(occurrences(payload->text, "display-message -p"), 16U);
  EXPECT_EQ(occurrences(payload->text, "display-message -p ''"), 4U);
  EXPECT_EQ(occurrences(payload->text, ":BEGIN"), 4U);
  EXPECT_EQ(occurrences(payload->text, "\\exit 0"), 4U);
  const std::size_t trap = payload->text.find("\\trap ");
  const std::size_t opening = payload->text.find(":BEGIN");
  const std::size_t saved_status = payload->text.find("\\set -- \"$?\"");
  const std::size_t closing = payload->text.find("\"$1\"");
  ASSERT_NE(trap, std::string::npos);
  ASSERT_NE(opening, std::string::npos);
  ASSERT_NE(saved_status, std::string::npos);
  ASSERT_NE(closing, std::string::npos);
  EXPECT_LT(trap, opening);
  EXPECT_LT(saved_status, closing);
}

TEST(McpTools, ShellCommandPayloadRetriesEveryCandidateCollision) {
  const std::string first(32U, '1');
  const std::string second(32U, '2');
  const std::string third(32U, '3');
  const std::string fourth(32U, '4');
  const auto marker = [](const std::string& nonce) {
    return "__LIBTMUX_MCP_DONE_" + nonce + "__";
  };
  const std::array<std::string, 7> candidates{
      "short", first, first, second, third, std::string(32U, 'A'), fourth};
  std::size_t calls = 0U;
  const auto payload = libtmux::mcp::detail::shell_command_payload(
      "command " + marker(first), "/tmp/" + marker(second) + "/tmux",
      "/tmp/" + marker(third) + ".sock", [&] { return candidates.at(calls++); });
  ASSERT_TRUE(payload.has_value()) << payload.error().message;
  EXPECT_EQ(calls, candidates.size());
  EXPECT_EQ(payload->marker, marker(fourth));
  EXPECT_EQ(payload->text.find(payload->marker), std::string::npos);

  calls = 0U;
  const auto exhausted = libtmux::mcp::detail::shell_command_payload(
      "true", "/opt/tmux", "/tmp/mcp.sock", [&] {
        ++calls;
        return std::string{"invalid"};
      });
  ASSERT_FALSE(exhausted.has_value());
  EXPECT_EQ(calls, 32U);
  EXPECT_NE(exhausted.error().message.find("collision-free"), std::string::npos);
}

TEST(McpTools, ShellCommandPayloadKeepsArbitraryCallerSyntaxInsideEval) {
  const std::string command = "printf output; echo tail\n'quote' # trailing\nexit 23";
  const auto payload = libtmux::mcp::detail::shell_command_payload(
      command, "/opt/tmux", "/tmp/mcp.sock", [] { return std::string(32U, 'b'); });
  ASSERT_TRUE(payload.has_value()) << payload.error().message;
  EXPECT_NE(payload->text.find("printf output"), std::string::npos);
  EXPECT_NE(payload->text.find("echo tail"), std::string::npos);
  EXPECT_NE(payload->text.find("# trailing"), std::string::npos);
  EXPECT_NE(payload->text.find("exit 23"), std::string::npos);
  EXPECT_EQ(payload->text.find(payload->marker), std::string::npos);
}

TEST(McpTools, ExecutableResolutionUsesPathOrderAndCanonicalFiles) {
  ExecutableTree tree;
  const std::filesystem::path first = tree.executable("first");
  static_cast<void>(tree.executable("second"));
  const auto resolved = libtmux::mcp::detail::resolve_executable(
      (tree.root() / "first").string() + ":" + (tree.root() / "second").string(),
      tree.root());
  ASSERT_TRUE(resolved.has_value()) << resolved.error().message;
  EXPECT_EQ(*resolved, std::filesystem::canonical(first).string());

  const std::filesystem::path current = tree.executable("current");
  const auto empty_component = libtmux::mcp::detail::resolve_executable(
      ":" + (tree.root() / "second").string(), current.parent_path());
  ASSERT_TRUE(empty_component.has_value()) << empty_component.error().message;
  EXPECT_EQ(*empty_component, std::filesystem::canonical(current).string());
}

TEST(McpTools, ExecutableResolutionSkipsUnusableEntriesAndHasNoFixedFallback) {
  ExecutableTree tree;
  static_cast<void>(tree.executable("not-executable", false));
  std::filesystem::create_directories(tree.root() / "directory" / "tmux");
  const std::filesystem::path actual = tree.executable("cs-path");
  const std::filesystem::path link = tree.root() / "linked" / "tmux";
  std::filesystem::create_directories(link.parent_path());
  std::filesystem::create_symlink(actual, link);
  const std::string search = (tree.root() / "not-executable").string() + ":" +
                             (tree.root() / "directory").string() + ":" +
                             (tree.root() / "linked").string();
  const auto resolved = libtmux::mcp::detail::resolve_executable(search, tree.root());
  ASSERT_TRUE(resolved.has_value()) << resolved.error().message;
  EXPECT_EQ(*resolved, std::filesystem::canonical(actual).string());

  const auto cs_path = libtmux::mcp::detail::resolve_executable(
      (tree.root() / "cs-path").string(), tree.root());
  ASSERT_TRUE(cs_path.has_value()) << cs_path.error().message;
  EXPECT_EQ(*cs_path, std::filesystem::canonical(actual).string());
  EXPECT_FALSE(
      libtmux::mcp::detail::resolve_executable("", tree.root() / "empty").has_value());
}

TEST(McpTools, ShellCommandCompletionRequiresAnExactBoundedStatusRecord) {
  const std::string marker = "__LIBTMUX_MCP_DONE_nonce__";
  const std::string prefix = "old\n\n" + marker + ":BEGIN\noutput\n";
  const std::array<std::pair<std::string_view, std::optional<int>>, 9> cases{{
      {"0", 0},
      {"23", 23},
      {"255", 255},
      {"", std::nullopt},
      {"-1", std::nullopt},
      {"23x", std::nullopt},
      {"23\t", std::nullopt},
      {"256", std::nullopt},
      {"%s", std::nullopt},
  }};
  for (const auto& [status, expected] : cases) {
    const std::string capture =
        prefix + "\n" + marker + ":" + std::string{status} + "\nprompt";
    const auto parsed = libtmux::mcp::detail::shell_command_completion(
        capture, marker, std::string_view{"old\n"}.size());
    ASSERT_EQ(parsed.has_value(), expected.has_value()) << status;
    if (expected.has_value()) {
      EXPECT_EQ(parsed->exit_code, *expected);
      EXPECT_EQ(
          capture.substr(parsed->text_begin, parsed->record_begin - parsed->text_begin),
          "output\n");
    }
  }

  const std::string evicted = "retained\n\n" + marker + ":23\nprompt";
  const auto parsed_evicted =
      libtmux::mcp::detail::shell_command_completion(evicted, marker);
  ASSERT_TRUE(parsed_evicted.has_value());
  EXPECT_EQ(parsed_evicted->exit_code, 23);
  EXPECT_EQ(evicted.substr(parsed_evicted->text_begin,
                           parsed_evicted->record_begin - parsed_evicted->text_begin),
            "retained\n");
  const std::string padded =
      "old\n\n" + marker + ":BEGIN   \noutput\n\n" + marker + ":23  \nprompt";
  const auto parsed_padded =
      libtmux::mcp::detail::shell_command_completion(padded, marker);
  ASSERT_TRUE(parsed_padded.has_value());
  EXPECT_EQ(parsed_padded->exit_code, 23);
  EXPECT_EQ(padded.substr(parsed_padded->text_begin,
                          parsed_padded->record_begin - parsed_padded->text_begin),
            "output\n");
  EXPECT_FALSE(libtmux::mcp::detail::shell_command_completion(
                   "\n" + marker + ":BEGIN\n\n" + marker + ":0", marker)
                   .has_value());
  EXPECT_FALSE(libtmux::mcp::detail::shell_command_completion(
                   "\n" + marker + ":BEGIN\t\noutput\n\n" + marker + ":0\n", marker)
                   .has_value());
  EXPECT_FALSE(libtmux::mcp::detail::shell_command_completion(
                   "\n" + marker + "_lookalike:0\n", marker)
                   .has_value());
  const std::string old_record = "\n" + marker + ":23\nretained";
  EXPECT_FALSE(libtmux::mcp::detail::shell_command_completion(
                   old_record, marker, old_record.find("retained"))
                   .has_value());
}

TEST(McpToolsTmux, PaneInputPreflightReadsFreshState) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto panes = server.panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  ASSERT_EQ(panes->size(), 1U);
  const libtmux::Pane pane = panes->front();
  EXPECT_FALSE(pane.in_mode());

  ASSERT_TRUE(pane.enter_copy_mode().has_value());
  const auto guarded = libtmux::mcp::detail::preflight_pane_input(
      server, pane.id(), libtmux::mcp::detail::PaneInputScope::target_only);
  ASSERT_FALSE(guarded.has_value());
  EXPECT_NE(guarded.error().message.find(pane.id()), std::string::npos);
  EXPECT_EQ(guarded.error().message.find("scripted expansion failure"),
            std::string::npos);
}

TEST(McpToolsTmux, CreatesAWindowAndTypesIntoItsPane) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const auto tools = all_tools();
  const std::string session{fixture->session_name()};

  const auto created = tools.call(
      server, "create_window", Arguments{{"session", session}, {"name", "from-mcp"}});
  ASSERT_TRUE(created.has_value()) << created.error().message;
  const std::string window_id = string_field(*created, "window_id");
  EXPECT_EQ(window_id.front(), '@');

  const auto typed = tools.call(server, "paste_text",
                                Arguments{{"paneId", window_id}, {"text", "marker"}});
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

TEST(McpToolsTmux, LiteralizesTmuxFormatBearingStateOnce) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto windows = server.windows();
  ASSERT_TRUE(windows.has_value()) << windows.error().diagnostic;
  ASSERT_EQ(windows->size(), 1U);
  const std::string window_id{windows->front().id()};
  const std::string literal_name{"literal-#{session_name}"};
  const auto tools = all_tools();

  const auto renamed =
      tools.call(server, "rename_window",
                 Arguments{{"windowId", window_id}, {"name", literal_name}});
  ASSERT_TRUE(renamed.has_value()) << renamed.error().message;
  const auto inspected =
      tools.call(server, "get_window_info", Arguments{{"windowId", window_id}});
  ASSERT_TRUE(inspected.has_value()) << inspected.error().message;
  const auto& window =
      std::get<StructuredValue::Object>(inspected->structured.at("window").value);
  EXPECT_EQ(std::get<std::string>(window.at("name").value), literal_name);
}

TEST(McpTools, EveryToolDeclaresANameAndDescription) {
  // The set must be named: tools() returns a reference into it, and a
  // range-for over the temporary would outlive what it borrows.
  const auto tools = all_tools();
  for (const auto& tool : tools.tools()) {
    EXPECT_FALSE(tool.name.empty());
    EXPECT_FALSE(tool.description.empty());
  }
  EXPECT_EQ(tools.tools().size(), 45U);
}

TEST(McpTools, CapabilityManifestMatchesThePinnedCrossPortInventory) {
  const auto tools = all_tools();
  const std::array<std::pair<std::string_view, Toolset>, 45> expected{{
      {"list_sessions", Toolset::inspect},
      {"list_windows", Toolset::inspect},
      {"list_panes", Toolset::inspect},
      {"get_server_info", Toolset::inspect},
      {"get_session_info", Toolset::inspect},
      {"get_window_info", Toolset::inspect},
      {"get_pane_info", Toolset::inspect},
      {"capture_pane", Toolset::inspect},
      {"capture_since", Toolset::inspect},
      {"snapshot_pane", Toolset::inspect},
      {"search_panes", Toolset::inspect},
      {"find_pane_by_position", Toolset::inspect},
      {"wait_for_text", Toolset::inspect},
      {"get_tmux_variables", Toolset::inspect},
      {"show_option", Toolset::inspect},
      {"show_environment", Toolset::inspect},
      {"show_hooks", Toolset::inspect},
      {"call_read_tools_batch", Toolset::inspect},
      {"rename_session", Toolset::manage},
      {"rename_window", Toolset::manage},
      {"select_window", Toolset::manage},
      {"select_pane", Toolset::manage},
      {"select_layout", Toolset::manage},
      {"resize_window", Toolset::manage},
      {"resize_pane", Toolset::manage},
      {"move_window", Toolset::manage},
      {"swap_pane", Toolset::manage},
      {"set_pane_title", Toolset::manage},
      {"wait_for_channel", Toolset::manage},
      {"signal_channel", Toolset::manage},
      {"set_mouse_enabled", Toolset::manage},
      {"set_history_limit", Toolset::manage},
      {"create_session", Toolset::execute},
      {"create_window", Toolset::execute},
      {"split_window", Toolset::execute},
      {"respawn_pane", Toolset::execute},
      {"run_shell_command", Toolset::execute},
      {"send_keys", Toolset::execute},
      {"send_keys_batch", Toolset::execute},
      {"paste_text", Toolset::execute},
      {"set_synchronize_panes", Toolset::execute},
      {"clear_pane_scrollback", Toolset::teardown},
      {"kill_pane", Toolset::teardown},
      {"kill_window", Toolset::teardown},
      {"kill_session", Toolset::teardown},
  }};

  ASSERT_EQ(tools.tools().size(), expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    EXPECT_EQ(tools.tools()[index].name, expected[index].first) << index;
    EXPECT_EQ(tools.tools()[index].toolset, expected[index].second) << index;
  }

  const auto schema_names = [&tools](std::string_view name) {
    std::set<std::string, std::less<>> result;
    const ToolDefinition* const tool = tools.find(name);
    EXPECT_NE(tool, nullptr) << name;
    if (tool != nullptr) {
      for (const auto& parameter : tool->schema.input) {
        result.insert(parameter.name);
      }
    }
    return result;
  };
  EXPECT_EQ(schema_names("create_session"),
            (std::set<std::string, std::less<>>{"height", "name", "startDirectory",
                                                "width", "windowName"}));
  EXPECT_EQ(schema_names("create_window"),
            (std::set<std::string, std::less<>>{"name", "session", "startDirectory"}));
  EXPECT_EQ(
      schema_names("split_window"),
      (std::set<std::string, std::less<>>{"direction", "paneId", "startDirectory"}));
  EXPECT_EQ(schema_names("respawn_pane"),
            (std::set<std::string, std::less<>>{"force", "killFirst", "paneId",
                                                "startDirectory"}));
  for (const std::string_view name :
       {"create_session", "create_window", "split_window", "respawn_pane"}) {
    const ToolDefinition* const tool = tools.find(name);
    ASSERT_NE(tool, nullptr) << name;
    EXPECT_EQ(tool->toolset, Toolset::execute);
    EXPECT_EQ(tool->authority.process_reach, ProcessReach::configured_process);
  }
  const ToolDefinition* const batch = tools.find("call_read_tools_batch");
  ASSERT_NE(batch, nullptr);
  EXPECT_TRUE(batch->description.starts_with(
      "Read pane output; accepts no client-supplied executable input. Returned "
      "content may be sensitive or untrusted."));
  const ToolDefinition* const capture_since = tools.find("capture_since");
  ASSERT_NE(capture_since, nullptr);
  EXPECT_EQ(capture_since->authority.effects, std::set{Effect::observe});
  EXPECT_EQ(batch->authority.effects, std::set{Effect::observe});
  for (const auto& [tool_name, field_name] :
       {std::pair{"rename_session", "name"}, std::pair{"rename_window", "name"},
        std::pair{"set_pane_title", "title"}}) {
    const ToolDefinition* const tool = tools.find(tool_name);
    ASSERT_NE(tool, nullptr) << tool_name;
    EXPECT_EQ(tool->authority.input_sinks.at(field_name),
              (std::set{Sink{InputSink::tmux_state, NestedAuthority::none},
                        Sink{InputSink::tmux_format, NestedAuthority::controlled,
                             InputControl::double_hash_once}}))
        << tool_name;
  }
}

TEST(McpTools, CapabilityRegistryOwnsTheCurrentSurface) {
  const auto built = default_tools(ToolSelection::all());
  ASSERT_TRUE(built.has_value()) << built.error();
  const ToolRegistry& tools = *built;
  ASSERT_EQ(tools.tools().size(), 45U);

  struct Expected {
    std::string_view name;
    Toolset toolset;
    ProcessReach reach;
    std::set<Effect> effects;
    std::set<OutputClass> outputs;
    bool secrets;
    bool untrusted;
    std::string_view opener;
  };
  const std::array expected{
      Expected{"list_sessions",
               Toolset::inspect,
               ProcessReach::none,
               {Effect::observe},
               {OutputClass::tmux_metadata},
               false,
               false,
               "Inspect tmux metadata; accepts no client-supplied executable input."},
      Expected{"capture_pane",
               Toolset::inspect,
               ProcessReach::none,
               {Effect::observe},
               {OutputClass::tmux_metadata, OutputClass::terminal_content},
               true,
               true,
               "Read pane output; accepts no client-supplied executable input. "
               "Returned content may be sensitive or untrusted."},
      Expected{"show_option",
               Toolset::inspect,
               ProcessReach::none,
               {Effect::observe},
               {OutputClass::tmux_metadata, OutputClass::configured_command},
               false,
               true,
               "Read configured tmux commands; accepts no client-supplied executable "
               "input. Returned values may contain executable configuration."},
      Expected{"create_session",
               Toolset::execute,
               ProcessReach::configured_process,
               {Effect::observe, Effect::change},
               {OutputClass::tmux_metadata},
               false,
               false,
               "Start a pane's configured process; accepts no command payload."},
      Expected{"paste_text",
               Toolset::execute,
               ProcessReach::pane_input,
               {Effect::observe, Effect::change},
               {OutputClass::tmux_metadata},
               false,
               true,
               "Send input to a pane's program; a shell that receives it runs it "
               "with your user's permissions."},
  };
  for (const Expected& item : expected) {
    const ToolDefinition* const tool = tools.find(item.name);
    ASSERT_NE(tool, nullptr) << item.name;
    EXPECT_EQ(tool->toolset, item.toolset);
    EXPECT_EQ(tool->authority.process_reach, item.reach);
    EXPECT_EQ(tool->authority.effects, item.effects);
    EXPECT_EQ(tool->authority.output_classes, item.outputs);
    EXPECT_EQ(tool->authority.may_expose_secrets, item.secrets);
    EXPECT_EQ(tool->authority.may_return_untrusted_content, item.untrusted);
    EXPECT_TRUE(tool->description.starts_with(item.opener));
    EXPECT_FALSE(tool->annotations.read_only);
    EXPECT_TRUE(tool->annotations.destructive);
    EXPECT_FALSE(tool->annotations.idempotent);
    EXPECT_TRUE(tool->annotations.open_world);
  }
}

TEST(McpTools, CapabilityRegistryRejectsInvalidDefinitions) {
  const auto built = default_tools(ToolSelection::all());
  ASSERT_TRUE(built.has_value()) << built.error();
  const ToolDefinition baseline = *built->find("paste_text");

  const auto rejected = [](ToolDefinition tool, std::string_view expected) {
    auto registry = ToolRegistry::create({std::move(tool)}, ToolSelection::all());
    ASSERT_FALSE(registry.has_value());
    EXPECT_NE(registry.error().find(expected), std::string::npos) << registry.error();
  };

  ToolDefinition invalid = baseline;
  invalid.authority.effects.clear();
  rejected(invalid, "effects must not be empty");
  invalid = baseline;
  invalid.schema.input.push_back(invalid.schema.input.front());
  invalid.schema.input.back().name = "missing";
  rejected(invalid, "missing input sinks: missing");
  invalid = baseline;
  invalid.authority.input_sinks.emplace(
      "extra", std::set{Sink{InputSink::none, NestedAuthority::none}});
  rejected(invalid, "extra input sinks: extra");
  invalid = baseline;
  invalid.authority.input_sinks.at("text").clear();
  rejected(invalid, "input sink set must not be empty: text");
  invalid = baseline;
  invalid.toolset = static_cast<Toolset>(255);
  rejected(invalid, "invalid toolset");
  invalid = baseline;
  invalid.authority.process_reach = static_cast<ProcessReach>(255);
  rejected(invalid, "invalid process reach");
  invalid = baseline;
  invalid.authority.effects = {static_cast<Effect>(255)};
  rejected(invalid, "invalid effect");
  invalid = baseline;
  invalid.authority.effects = {Effect::observe};
  rejected(invalid, "toolset/effect mismatch");
  invalid = baseline;
  invalid.authority.output_classes = {static_cast<OutputClass>(255)};
  rejected(invalid, "invalid output class");
  invalid = baseline;
  invalid.schema.input.front().type = static_cast<libtmux::mcp::ArgumentType>(255);
  rejected(invalid, "invalid schema field type");
  invalid = baseline;
  invalid.schema.output = static_cast<OutputShape>(255);
  rejected(invalid, "invalid output schema");
  invalid = baseline;
  invalid.authority.input_sinks.at("text") = {
      Sink{static_cast<InputSink>(255), NestedAuthority::none}};
  rejected(invalid, "invalid input sink");
  invalid = baseline;
  invalid.authority.input_sinks.at("text") = {
      Sink{InputSink::pane_input, static_cast<NestedAuthority>(255)}};
  rejected(invalid, "invalid nested authority");
  invalid = baseline;
  invalid.authority.input_sinks.at("paneId") = {
      Sink{InputSink::tmux_lookup, NestedAuthority::controlled}};
  rejected(invalid, "non-executable sink has nested authority");
  invalid = baseline;
  invalid.authority.input_sinks.at("text") = {
      Sink{InputSink::pane_input, NestedAuthority::none}};
  rejected(invalid, "executable sink lacks nested authority");
  invalid = baseline;
  invalid.authority.input_sinks.at("text") = {
      Sink{InputSink::tmux_format, NestedAuthority::unrestricted}};
  rejected(invalid, "unrestricted tmux-format is prohibited");
  invalid = baseline;
  invalid.authority.process_reach = ProcessReach::configured_process;
  rejected(invalid, "configured-process accepts no executable input sink");
  invalid = baseline;
  invalid.authority.process_reach = ProcessReach::none;
  rejected(invalid, "pane-input reach/sink mismatch");
  invalid = baseline;
  invalid.description = "Type pane text.";
  rejected(invalid, "description has the wrong controlled opener");
  invalid = baseline;
  invalid.annotations.read_only = true;
  rejected(invalid, "annotations must be conservative");
  invalid = baseline;
  invalid.handler = {};
  rejected(invalid, "handler is missing");

  auto duplicate = ToolRegistry::create({baseline, baseline}, ToolSelection::all());
  ASSERT_FALSE(duplicate.has_value());
  EXPECT_NE(duplicate.error().find("duplicate tool: paste_text"), std::string::npos);

  ToolSelection invalid_selection;
  invalid_selection.toolsets = {static_cast<Toolset>(255)};
  const auto bad_selection = ToolRegistry::create({baseline}, invalid_selection);
  ASSERT_FALSE(bad_selection.has_value());
  EXPECT_NE(bad_selection.error().find("invalid selected toolset"), std::string::npos);
}

TEST(McpTools, CapabilityRegistryResolvesAllToolsetSubsetsAndNames) {
  const std::array<std::string_view, 4> toolsets{"inspect", "manage", "execute",
                                                 "teardown"};
  const auto names = [](const ToolRegistry& registry) {
    std::vector<std::string> result;
    for (const ToolDefinition& tool : registry.tools()) {
      result.push_back(tool.name);
    }
    return result;
  };
  for (unsigned mask = 0; mask < 16U; ++mask) {
    std::string forward;
    std::string reverse;
    for (unsigned bit = 0; bit < toolsets.size(); ++bit) {
      if ((mask & (1U << bit)) != 0U) {
        if (!forward.empty()) {
          forward += ',';
        }
        forward += toolsets[bit];
        if (!reverse.empty()) {
          reverse.insert(0, ",");
        }
        reverse.insert(0, toolsets[bit]);
      }
    }
    const auto selection = libtmux::mcp::parse_tool_selection(
        std::optional<std::string_view>{forward}, std::nullopt, std::nullopt);
    const auto reversed = libtmux::mcp::parse_tool_selection(
        std::optional<std::string_view>{reverse}, std::nullopt, std::nullopt);
    ASSERT_TRUE(selection.has_value()) << selection.error();
    ASSERT_TRUE(reversed.has_value()) << reversed.error();
    const auto selected = default_tools(*selection);
    const auto selected_reversed = default_tools(*reversed);
    ASSERT_TRUE(selected.has_value()) << selected.error();
    ASSERT_TRUE(selected_reversed.has_value()) << selected_reversed.error();
    const std::size_t expected =
        ((mask & 1U) != 0U ? 18U : 0U) + ((mask & 2U) != 0U ? 14U : 0U) +
        ((mask & 4U) != 0U ? 9U : 0U) + ((mask & 8U) != 0U ? 4U : 0U);
    EXPECT_EQ(selected->tools().size(), expected) << mask;
    EXPECT_EQ(names(*selected), names(*selected_reversed)) << mask;
  }

  ToolSelection selection;
  selection.include = {"capture_pane", "paste_text"};
  selection.exclude = {"paste_text"};
  const auto selected = default_tools(selection);
  ASSERT_TRUE(selected.has_value()) << selected.error();
  ASSERT_EQ(selected->tools().size(), 1U);
  EXPECT_EQ(selected->tools().front().name, "capture_pane");
  EXPECT_NE(selected->find("capture_pane"), nullptr);
  EXPECT_EQ(selected->find("paste_text"), nullptr);

  selection.include = {"unknown"};
  EXPECT_FALSE(default_tools(selection).has_value());
  selection.include.clear();
  selection.exclude = {"unknown"};
  EXPECT_FALSE(default_tools(selection).has_value());
}

TEST(McpTools, CapabilitySelectionParsesEmptyAndRejectsMalformedLists) {
  using OptionalText = std::optional<std::string_view>;
  const auto empty =
      libtmux::mcp::parse_tool_selection(OptionalText{""}, std::nullopt, std::nullopt);
  ASSERT_TRUE(empty.has_value()) << empty.error();
  EXPECT_TRUE(empty->toolsets.empty());

  const auto defaults =
      libtmux::mcp::parse_tool_selection(std::nullopt, std::nullopt, std::nullopt);
  ASSERT_TRUE(defaults.has_value()) << defaults.error();
  EXPECT_EQ(defaults->toolsets, (std::set{Toolset::inspect, Toolset::manage,
                                          Toolset::execute, Toolset::teardown}));

  const auto shared_defaults = libtmux::mcp::parse_tool_selection(
      std::nullopt, std::nullopt, std::nullopt, false);
  ASSERT_TRUE(shared_defaults.has_value()) << shared_defaults.error();
  EXPECT_EQ(shared_defaults->toolsets,
            (std::set{Toolset::inspect, Toolset::manage, Toolset::execute}));
  const auto explicit_teardown = libtmux::mcp::parse_tool_selection(
      OptionalText{"inspect,teardown"}, std::nullopt, std::nullopt, false);
  ASSERT_TRUE(explicit_teardown.has_value()) << explicit_teardown.error();
  EXPECT_TRUE(explicit_teardown->toolsets.contains(Toolset::teardown));

  for (const std::string_view malformed :
       {"inspect,", ",inspect", "inspect,,execute", "unknown"}) {
    EXPECT_FALSE(libtmux::mcp::parse_tool_selection(OptionalText{malformed},
                                                    std::nullopt, std::nullopt)
                     .has_value())
        << malformed;
  }
  EXPECT_FALSE(
      libtmux::mcp::parse_tool_selection(std::nullopt, OptionalText{""}, std::nullopt)
          .has_value());
  EXPECT_FALSE(libtmux::mcp::parse_tool_selection(std::nullopt, std::nullopt,
                                                  OptionalText{"capture_pane,"})
                   .has_value());
}

TEST(McpToolsTmux, ReadBatchReturnsPartialRowsAndHonorsContinue) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  ToolSelection selection;
  selection.include = {"call_read_tools_batch"};
  auto built = default_tools(selection);
  ASSERT_TRUE(built.has_value()) << built.error();
  Arguments arguments{{"onError", "continue"}};
  arguments.read_calls = {{"get_session_info", {}}, {"list_sessions", {}}};

  const auto result =
      built->call(connect(*fixture), "call_read_tools_batch", arguments);

  ASSERT_TRUE(result.has_value()) << result.error().message;
  EXPECT_EQ(std::get<std::int64_t>(result->structured.at("failed").value), 1);
  EXPECT_EQ(std::get<std::int64_t>(result->structured.at("succeeded").value), 1);
  EXPECT_EQ(std::get<std::string>(result->structured.at("onError").value), "continue");
  const auto& rows =
      std::get<StructuredValue::Array>(result->structured.at("results").value);
  ASSERT_EQ(rows.size(), 2U);
  const auto& failed_row = std::get<StructuredValue::Object>(rows[0].value);
  EXPECT_FALSE(std::get<bool>(failed_row.at("success").value));
  const auto& nested_error =
      std::get<StructuredValue::Object>(failed_row.at("result").value);
  EXPECT_TRUE(std::get<bool>(nested_error.at("isError").value));
  EXPECT_TRUE(
      std::holds_alternative<StructuredValue::Array>(nested_error.at("content").value));
  EXPECT_TRUE(std::get<bool>(
      std::get<StructuredValue::Object>(rows[1].value).at("success").value));
}

TEST(McpTools, ReadBatchCarriesCompleteRowsToTheProtocolLimiter) {
  auto complete = default_tools(ToolSelection::all());
  ASSERT_TRUE(complete.has_value()) << complete.error();
  const ToolDefinition* const source_batch = complete->find("call_read_tools_batch");
  ASSERT_NE(source_batch, nullptr);
  std::vector<ToolDefinition> definitions{*source_batch};
  const std::string payload(600U * 1024U, 'x');
  for (const std::string& name : source_batch->authority.nested_tools) {
    ToolDefinition nested = *complete->find(name);
    nested.schema.input.clear();
    nested.authority.input_sinks.clear();
    nested.handler = [payload](const Server&, const Arguments&,
                               const libtmux::mcp::CallContext&) -> ToolResult {
      return ToolOutput{.structured = {{"payload", payload}}};
    };
    definitions.push_back(std::move(nested));
  }
  ToolSelection selection;
  selection.include = {"call_read_tools_batch"};
  auto built = ToolRegistry::create(std::move(definitions), selection);
  ASSERT_TRUE(built.has_value()) << built.error();
  Arguments arguments;
  arguments.read_calls = {{"list_sessions", {}}, {"list_windows", {}}};
  auto server = Server::at_socket_name("mcp-batch-bounds");
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;

  const auto result = built->call(*server, "call_read_tools_batch", arguments);

  ASSERT_TRUE(result.has_value()) << result.error().message;
  ASSERT_TRUE(result->maximum_response_bytes.has_value());
  EXPECT_EQ(*result->maximum_response_bytes, 1'000'000U);
  EXPECT_FALSE(std::get<bool>(result->structured.at("truncated").value));
  EXPECT_EQ(std::get<std::int64_t>(result->structured.at("truncatedBytes").value), 0);
  const auto& rows =
      std::get<StructuredValue::Array>(result->structured.at("results").value);
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_TRUE(std::ranges::all_of(rows, [](const StructuredValue& value) {
    return !std::get<bool>(
        std::get<StructuredValue::Object>(value.value).at("resultTruncated").value);
  }));
  EXPECT_EQ(std::get<std::int64_t>(result->structured.at("succeeded").value), 2);
}

TEST(McpTools, CountsUtf8CodePointsLikeThePublishedSchema) {
  auto built = ToolRegistry::create(
      {ToolDefinition{
          .name = "unicode",
          .title = "Unicode",
          .description =
              "Inspect tmux metadata; accepts no client-supplied executable input. "
              "Validate a bounded Unicode string.",
          .toolset = Toolset::inspect,
          .authority = {.process_reach = ProcessReach::none,
                        .effects = {Effect::observe},
                        .output_classes = {OutputClass::tmux_metadata},
                        .may_expose_secrets = false,
                        .may_return_untrusted_content = true,
                        .amplifies_future_input = false,
                        .input_sinks = {{"value",
                                         {{InputSink::none, NestedAuthority::none}}}},
                        .nested_tools = {}},
          .annotations = libtmux::mcp::kConservativeAnnotations,
          .schema = {.input = {{.name = "value",
                                .description = "At most two characters.",
                                .type = libtmux::mcp::ArgumentType::string,
                                .required = true,
                                .minimum = std::nullopt,
                                .maximum = std::nullopt,
                                .maximum_length = 2U,
                                .allowed_values = {}}},
                     .output = OutputShape::pane_text},
          .handler = [](const Server&, const Arguments&,
                        const libtmux::mcp::CallContext&) -> ToolResult {
            return ToolOutput{.structured = {}};
          }}},
      ToolSelection::all());
  ASSERT_TRUE(built.has_value()) << built.error();
  ToolRegistry tools = std::move(*built);
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

TEST(McpTools, ValidatesEveryNestedSendKeysBatchRowBeforeDispatch) {
  auto complete = default_tools(ToolSelection::all());
  ASSERT_TRUE(complete.has_value()) << complete.error();
  const ToolDefinition* const source = complete->find("send_keys_batch");
  ASSERT_NE(source, nullptr);
  ToolDefinition batch = *source;
  std::size_t dispatches = 0U;
  batch.handler = [&dispatches](const Server&, const Arguments&,
                                const libtmux::mcp::CallContext&) -> ToolResult {
    ++dispatches;
    return ToolOutput{.structured = {}};
  };
  auto built = ToolRegistry::create({std::move(batch)}, ToolSelection::all());
  ASSERT_TRUE(built.has_value()) << built.error();
  auto server = Server::at_socket_name("mcp-send-key-batch-validation");
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;

  const std::string face{"\xF0\x9F\x98\x80"};
  const auto repeated = [&face](std::size_t count) {
    std::string value;
    value.reserve(face.size() * count);
    for (std::size_t index = 0U; index < count; ++index) {
      value += face;
    }
    return value;
  };
  const auto call = [&](FlatArguments operation) {
    Arguments arguments;
    arguments.send_key_operations.push_back(std::move(operation));
    return built->call(*server, "send_keys_batch", arguments);
  };

  const auto exact = call({{"paneId", repeated(512U)},
                           {"keys", repeated(4096U)},
                           {"enter", "true"},
                           {"literal", "false"}});
  ASSERT_TRUE(exact.has_value()) << exact.error().message;
  EXPECT_EQ(dispatches, 1U);

  const std::vector<FlatArguments> refused{
      {{"paneId", repeated(513U)}, {"keys", "Escape"}},
      {{"paneId", "%1"}, {"keys", repeated(4097U)}},
      {{"paneId", "%1"},
       {"keys", std::string{static_cast<char>(0xC0), static_cast<char>(0x80)}}},
      {{"paneId", "%1"}, {"keys", "Escape"}, {"force", "true"}},
      {{"paneId", "%1"}, {"keys", "Escape"}, {"literal", "yes"}},
      {{"paneId", "%1"}},
      {{"keys", "Escape"}},
  };
  for (const FlatArguments& operation : refused) {
    const auto rejected = call(operation);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_TRUE(rejected.error().caller_error);
    EXPECT_EQ(dispatches, 1U) << rejected.error().message;
  }
}

TEST(McpTools, AppliesOneDeadlineToWaitStartupAndPolling) {
  using namespace std::chrono_literals;
  auto backend = std::make_shared<DeadlineBackend>(6ms);
  const Server server = libtmux::detail::server_over(backend);
  const auto started = std::chrono::steady_clock::now();
  const auto waited = all_tools().call(
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
  const auto waited = all_tools().call(
      server, "wait_for_text",
      {{"target", "mcp"}, {"text", "never appears"}, {"timeout_ms", "1000"}},
      libtmux::mcp::CallContext{
          .is_cancelled = [&cancelled] { return cancelled.load(); }});
  const auto elapsed = std::chrono::steady_clock::now() - started;
  cancel.join();

  ASSERT_FALSE(waited.has_value());
  EXPECT_FALSE(waited.error().caller_error);
  EXPECT_EQ(waited.error().message, "request cancelled");
  // Well inside the 1000ms budget is the claim: cancellation ended the wait
  // rather than the deadline. A tighter bound measures the machine instead.
  EXPECT_LT(elapsed, 500ms);
}

TEST(McpTools, RejectsSearchWhenDeterministicWorkBudgetIsSpent) {
  auto pattern = libtmux::mcp::detail::BoundedRegex::compile("a*a*a*a");
  ASSERT_TRUE(pattern.has_value()) << pattern.error();
  std::size_t remaining_work = 7U;

  const auto matched = pattern->search("aaaaaaaa", remaining_work);

  ASSERT_FALSE(matched.has_value());
  EXPECT_EQ(matched.error(), "search matching work limit exceeded");
  EXPECT_EQ(remaining_work, 3U);
}

TEST(McpTools, RejectsWaitWhenDeterministicMatchingWorkBudgetIsSpent) {
  auto backend = std::make_shared<DeadlineBackend>(
      std::chrono::milliseconds{0}, std::string(9U * 1024U * 1024U, 'x'));
  const Server server = libtmux::detail::server_over(backend);

  const auto waited = all_tools().call(
      server, "wait_for_text",
      {{"target", "mcp"}, {"text", "never appears"}, {"timeout_ms", "1000"}});

  ASSERT_FALSE(waited.has_value());
  EXPECT_FALSE(waited.error().caller_error);
  EXPECT_EQ(waited.error().message, "wait matching work limit exceeded");
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
      all_tools().call(*server, "search_panes", Arguments{{"pattern", "anything"}});
  ASSERT_FALSE(searched.has_value());
  EXPECT_FALSE(searched.error().caller_error);
  EXPECT_FALSE(searched.error().message.empty());
}

} // namespace
