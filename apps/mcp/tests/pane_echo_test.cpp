#include "libtmux_consumers/mcp.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "libtmux/capture.hpp"
#include "libtmux/server.hpp"
#include "libtmux/testing/scoped_server.hpp"
#include "pane_echo.hpp"

namespace {

using libtmux::Server;
using libtmux::mcp::Arguments;
using libtmux::mcp::StructuredValue;
using libtmux::mcp::ToolOutput;
using libtmux::mcp::ToolRegistry;

Server connect(const libtmux::test::ScopedTmuxServer& fixture) {
  auto server = Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value());
  return server.value();
}

const std::string& string_field(const ToolOutput& output, std::string_view name) {
  return std::get<std::string>(output.structured.at(std::string{name}).value);
}

bool has_output_line(const ToolOutput& output, std::string_view expected) {
  for (std::string_view line : libtmux::capture_lines(string_field(output, "text"))) {
    // tmux 3.2a retains trailing grid padding in joined captures.
    while (line.ends_with(' ')) {
      line.remove_suffix(1);
    }
    if (line == expected) {
      return true;
    }
  }
  return false;
}

ToolRegistry all_tools() {
  auto built = libtmux::mcp::default_tools();
  EXPECT_TRUE(built.has_value()) << built.error();
  return std::move(*built);
}

namespace echo_test {

using libtmux::mcp::detail::live_echo;
using libtmux::mcp::detail::LiveEcho;
using libtmux::mcp::detail::note_key_dispatch;
using libtmux::mcp::detail::note_literal_write;
using libtmux::mcp::detail::PaneServerIdentity;
using libtmux::mcp::detail::prune_dead_panes;

const PaneServerIdentity kIdentity{.socket_path = "/tmp/pane-echo-test.sock",
                                   .server_pid = 4242U,
                                   .server_start_time = 1000U};

int pane_counter = 0;
std::string fresh_pane() {
  ++pane_counter;
  return "%pane-echo-test-" + std::to_string(pane_counter);
}

} // namespace echo_test

TEST(McpTools, PaneEchoSubmissionStaysPendingUntilDispatchCompletes) {
  using namespace echo_test;
  const std::string pane = fresh_pane();
  auto edit = note_literal_write(kIdentity, pane, "echo MARKER", true);
  const LiveEcho during = live_echo(kIdentity, pane);
  EXPECT_TRUE(during.input_pending);
  EXPECT_EQ(during.recent, (std::vector<std::string>{"echo MARKER"}));
  edit.commit();
  EXPECT_FALSE(live_echo(kIdentity, pane).input_pending);
}

TEST(McpTools, PaneEchoDropsOversizedInputInsteadOfGrowingWithoutBound) {
  using namespace echo_test;
  const std::string pane = fresh_pane();
  auto edit =
      note_literal_write(kIdentity, pane, std::string(2U * 1024U * 1024U, 'x'), false);
  edit.commit();
  EXPECT_TRUE(live_echo(kIdentity, pane).pending.empty());
}

TEST(McpTools, PaneEchoExpiresRecentLinesAndIdlePanes) {
  using namespace echo_test;
  const std::string pane = fresh_pane();
  for (const std::string_view text : {"one", "two", "three", "four", "five"}) {
    auto edit = note_literal_write(kIdentity, pane, text, true);
    edit.commit();
  }
  EXPECT_EQ(live_echo(kIdentity, pane).recent,
            (std::vector<std::string>{"two", "three", "four", "five"}));
  auto edit = note_literal_write(kIdentity, pane, "pending", false);
  edit.commit();
  const auto now = std::chrono::steady_clock::now();
  const auto later = live_echo(kIdentity, pane, now + std::chrono::seconds{11});
  EXPECT_TRUE(later.recent.empty());
  EXPECT_EQ(later.pending, "pending");
  EXPECT_TRUE(
      live_echo(kIdentity, pane, now + std::chrono::minutes{3}).pending.empty());
}

TEST(McpTools, PaneEchoTracksLiteralTextAndClearsOnSubmit) {
  using namespace echo_test;
  const std::string pane = fresh_pane();
  {
    auto edit = note_literal_write(kIdentity, pane, "echo MARKER", false);
    edit.commit();
  }
  EXPECT_EQ(live_echo(kIdentity, pane).pending, "echo MARKER");
  {
    auto edit = note_key_dispatch(kIdentity, pane, "Enter", false);
    edit.commit();
  }
  const LiveEcho after = live_echo(kIdentity, pane);
  EXPECT_EQ(after.pending, "");
  ASSERT_EQ(after.recent.size(), 1U);
  EXPECT_EQ(after.recent.front(), "echo MARKER");
}

TEST(McpTools, PaneEchoBSpaceErasesExactlyIncludingOverflowIntoAnEarlierCall) {
  using namespace echo_test;
  const std::string pane = fresh_pane();
  {
    auto edit = note_key_dispatch(kIdentity, pane, "x", false);
    edit.commit();
  }
  for (const char letter : std::string_view{"MARKER"}) {
    auto edit = note_key_dispatch(kIdentity, pane, std::string(1, letter), false);
    edit.commit();
  }
  for (int i = 0; i < 7; ++i) {
    auto edit = note_key_dispatch(kIdentity, pane, "BSpace", false);
    edit.commit();
  }
  const LiveEcho midway = live_echo(kIdentity, pane);
  EXPECT_EQ(midway.pending, "");
  EXPECT_NE(std::ranges::find(midway.recent, "xMARKER"), midway.recent.end());
  auto edit = note_key_dispatch(kIdentity, pane, "BSpace", false);
  edit.commit();
  EXPECT_EQ(live_echo(kIdentity, pane).pending, "");
}

TEST(McpTools, PaneEchoKillLineKeysDiscardTheLineButStillProtectIt) {
  using namespace echo_test;
  for (const std::string_view kill_key : {"C-u", "C-c"}) {
    const std::string pane = fresh_pane();
    {
      auto edit = note_literal_write(kIdentity, pane, "oops", false);
      edit.commit();
    }
    auto edit = note_key_dispatch(kIdentity, pane, kill_key, false);
    edit.commit();
    const LiveEcho after = live_echo(kIdentity, pane);
    EXPECT_EQ(after.pending, "");
    EXPECT_NE(std::ranges::find(after.recent, "oops"), after.recent.end()) << kill_key;
  }
}

TEST(McpTools, PaneEchoDcIsANoOp) {
  using namespace echo_test;
  const std::string pane = fresh_pane();
  {
    auto edit = note_literal_write(kIdentity, pane, "MARKER", false);
    edit.commit();
  }
  auto edit = note_key_dispatch(kIdentity, pane, "DC", false);
  edit.commit();
  EXPECT_EQ(live_echo(kIdentity, pane).pending, "MARKER");
}

TEST(McpTools, PaneEchoUnmodelledKeyClearsTheLineRatherThanKeepingItStale) {
  using namespace echo_test;
  for (const std::string_view unknown_key :
       {"Left", "Right", "Home", "End", "Tab", "C-a", "F5"}) {
    const std::string pane = fresh_pane();
    {
      auto edit = note_literal_write(kIdentity, pane, "xMARKER", false);
      edit.commit();
    }
    auto edit = note_key_dispatch(kIdentity, pane, unknown_key, false);
    edit.commit();
    const LiveEcho after = live_echo(kIdentity, pane);
    EXPECT_EQ(after.pending, "") << unknown_key;
    EXPECT_TRUE(after.recent.empty()) << unknown_key;
  }
}

TEST(McpTools, PaneEchoSplitsSubmittedLinesAndRetainsTheUnsubmittedTail) {
  using namespace echo_test;
  const std::string pane = fresh_pane();
  auto edit = note_literal_write(kIdentity, pane, "echo one\necho two\rpartial", false);
  edit.commit();
  const LiveEcho after = live_echo(kIdentity, pane);
  EXPECT_EQ(after.pending, "partial");
  EXPECT_EQ(after.recent, (std::vector<std::string>{"echo one", "echo two"}));
}

TEST(McpTools, PaneEchoKeepsDifferentServersWithTheSamePaneId) {
  using namespace echo_test;
  const std::string pane = fresh_pane();
  PaneServerIdentity other = kIdentity;
  other.socket_path = "/tmp/other-pane-echo.sock";
  auto first = note_literal_write(kIdentity, pane, "first", false);
  first.commit();
  auto second = note_literal_write(other, pane, "second", false);
  second.commit();
  EXPECT_EQ(live_echo(kIdentity, pane).pending, "first");
  EXPECT_EQ(live_echo(other, pane).pending, "second");
  prune_dead_panes(other, {});
  EXPECT_EQ(live_echo(kIdentity, pane).pending, "first");
  EXPECT_TRUE(live_echo(other, pane).pending.empty());
}

TEST(McpTools, PaneEchoIsKeyedByServerIdentityNotPaneIdAlone) {
  using namespace echo_test;
  const std::string pane = fresh_pane();
  {
    auto edit = note_key_dispatch(kIdentity, pane, "y", false);
    edit.commit();
  }
  EXPECT_EQ(live_echo(kIdentity, pane).pending, "y");

  PaneServerIdentity restarted = kIdentity;
  restarted.server_start_time = 999999U;
  const LiveEcho stale = live_echo(restarted, pane);
  EXPECT_EQ(stale.pending, "");
  EXPECT_TRUE(stale.recent.empty());

  auto edit = note_key_dispatch(restarted, pane, "z", false);
  edit.commit();
  EXPECT_EQ(live_echo(restarted, pane).pending, "z");
}

TEST(McpTools, PaneEchoPruneDeadPanesBoundsMemoryOnceAPaneIsGone) {
  using namespace echo_test;
  const std::string pane = fresh_pane();
  {
    auto edit = note_literal_write(kIdentity, pane, "secret", false);
    edit.commit();
  }
  EXPECT_EQ(live_echo(kIdentity, pane).pending, "secret");
  prune_dead_panes(kIdentity, {});
  const LiveEcho after = live_echo(kIdentity, pane);
  EXPECT_EQ(after.pending, "");
  EXPECT_TRUE(after.recent.empty());
}

TEST(McpTools, PaneEchoEditRevertsUnlessCommitted) {
  using namespace echo_test;
  const std::string pane = fresh_pane();
  { auto edit = note_literal_write(kIdentity, pane, "will not be sent", false); }
  const LiveEcho after = live_echo(kIdentity, pane);
  EXPECT_EQ(after.pending, "");
  EXPECT_TRUE(after.recent.empty());
}

TEST(McpTools, PaneEchoEditRollbackRestoresThePriorLineNotAnEmptyOne) {
  using namespace echo_test;
  const std::string pane = fresh_pane();
  {
    auto edit = note_literal_write(kIdentity, pane, "kept", false);
    edit.commit();
  }
  { auto edit = note_literal_write(kIdentity, pane, " and lost", false); }
  EXPECT_EQ(live_echo(kIdentity, pane).pending, "kept");
}

namespace echo_contract_test {

void inject_foreign_output(const Server& server, const ToolRegistry& tools,
                           std::string_view writer_pane_id,
                           std::string_view target_pane_id, std::string_view line,
                           double delay_seconds) {
  const auto pane = server.pane(std::string{target_pane_id});
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;
  const auto tty = server.run(
      {"display-message", "-p", "-t", std::string{target_pane_id}, "#{pane_tty}"});
  ASSERT_TRUE(tty.has_value()) << tty.error().diagnostic;
  std::string tty_path = *tty;
  while (!tty_path.empty() && (tty_path.back() == '\n' || tty_path.back() == '\r')) {
    tty_path.pop_back();
  }
  ASSERT_FALSE(tty_path.empty());
  const auto answer =
      tools.call(server, "run_shell_command",
                 Arguments{{"paneId", std::string{writer_pane_id}},
                           {"command", "sleep " + std::to_string(delay_seconds) +
                                           " && printf '%s\\n' '" + std::string{line} +
                                           "' > " + tty_path},
                           {"timeoutMs", "3000"}});
  ASSERT_TRUE(answer.has_value()) << answer.error().message;
}

} // namespace echo_contract_test

TEST(McpToolsTmux, EchoShortUnsubmittedAnswerDoesNotMaskLongerRealOutput) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.new_session({.name = "echo-s1", .shell_command = "sh"});
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;
  auto panes = session->panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  ASSERT_FALSE(panes->empty());
  const std::string pane_id{panes->front().id().value()};
  const auto tools = all_tools();

  const auto split = tools.call(server, "split_window", Arguments{{"paneId", pane_id}});
  ASSERT_TRUE(split.has_value()) << split.error().message;
  const std::string writer_pane_id = string_field(*split, "pane_id");

  const auto typed =
      tools.call(server, "send_keys", Arguments{{"paneId", pane_id}, {"keys", "y"}});
  ASSERT_TRUE(typed.has_value()) << typed.error().message;

  std::thread injector{[&] {
    echo_contract_test::inject_foreign_output(server, tools, writer_pane_id, pane_id,
                                              "ready", 0.3);
  }};
  const auto waited = tools.call(
      server, "wait_for_text",
      Arguments{{"target", pane_id}, {"text", "ready"}, {"timeout_ms", "1000"}});
  injector.join();

  ASSERT_TRUE(waited.has_value()) << waited.error().message;
  EXPECT_TRUE(std::get<bool>(waited->structured.at("matched").value))
      << "the typed \"y\" masked real output \"ready\" that merely contains it; text=\""
      << string_field(*waited, "text") << "\"";
}

TEST(McpToolsTmux, EchoWaitBeforeSendMatchesOutputNotEcho) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.new_session({.name = "echo-s2a", .shell_command = "sh"});
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;
  auto panes = session->panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  ASSERT_FALSE(panes->empty());
  const std::string pane_id{panes->front().id().value()};
  const auto tools = all_tools();
  const std::string marker = "S2AMARKER";

  bool dispatched = false;
  libtmux::mcp::CallContext context;
  context.progress = [&](double, std::optional<double>, std::string) {
    if (dispatched) {
      return;
    }
    dispatched = true;
    Arguments batch;
    batch.send_key_operations.emplace_back(
        libtmux::mcp::FlatArguments{{"paneId", pane_id},
                                    {"keys", "sleep 0.1; echo " + marker},
                                    {"literal", "true"},
                                    {"enter", "true"}});
    const auto sent = tools.call(server, "send_keys_batch", batch);
    ASSERT_TRUE(sent.has_value()) << sent.error().message;
  };
  const auto waited = tools.call(
      server, "wait_for_text",
      Arguments{{"target", pane_id}, {"text", marker}, {"timeout_ms", "1000"}},
      context);
  EXPECT_TRUE(dispatched);

  ASSERT_TRUE(waited.has_value()) << waited.error().message;
  EXPECT_TRUE(std::get<bool>(waited->structured.at("matched").value));
  const std::string mode = string_field(*waited, "mode");
  EXPECT_NE(mode, "capture-at-entry") << string_field(*waited, "text");
  EXPECT_TRUE(has_output_line(*waited, marker)) << string_field(*waited, "text");
}

TEST(McpToolsTmux, EchoWaitAfterSendMatchesOutputNotEcho) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.new_session({.name = "echo-s2b", .shell_command = "sh"});
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;
  auto panes = session->panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  ASSERT_FALSE(panes->empty());
  const std::string pane_id{panes->front().id().value()};
  const auto tools = all_tools();
  const std::string marker = "S2BMARKER";

  const auto sent =
      tools.call(server, "paste_text",
                 Arguments{{"paneId", pane_id}, {"text", "sleep 0.1; echo " + marker}});
  ASSERT_TRUE(sent.has_value()) << sent.error().message;
  const auto enter = tools.call(server, "send_keys",
                                Arguments{{"paneId", pane_id}, {"keys", "Enter"}});
  ASSERT_TRUE(enter.has_value()) << enter.error().message;

  const auto waited = tools.call(
      server, "wait_for_text",
      Arguments{{"target", pane_id}, {"text", marker}, {"timeout_ms", "1000"}});

  ASSERT_TRUE(waited.has_value()) << waited.error().message;
  EXPECT_TRUE(std::get<bool>(waited->structured.at("matched").value));
  EXPECT_TRUE(has_output_line(*waited, marker)) << string_field(*waited, "text");
}

TEST(McpToolsTmux, EchoUnsubmittedTextTimesOut) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.new_session({.name = "echo-s3", .shell_command = "sh"});
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;
  auto panes = session->panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  ASSERT_FALSE(panes->empty());
  const std::string pane_id{panes->front().id().value()};
  const auto tools = all_tools();

  const auto typed = tools.call(
      server, "paste_text", Arguments{{"paneId", pane_id}, {"text", "echo MARKER"}});
  ASSERT_TRUE(typed.has_value()) << typed.error().message;

  const auto waited = tools.call(
      server, "wait_for_text",
      Arguments{{"target", pane_id}, {"text", "MARKER"}, {"timeout_ms", "1000"}});
  ASSERT_TRUE(waited.has_value()) << waited.error().message;
  EXPECT_TRUE(std::get<bool>(waited->structured.at("timed_out").value));
  EXPECT_FALSE(std::get<bool>(waited->structured.at("matched").value))
      << "matched the unsubmitted echo; text=\"" << string_field(*waited, "text")
      << "\"";
  EXPECT_TRUE(string_field(*waited, "mode").ends_with("-unconfirmed"));
}

TEST(McpToolsTmux, EchoBeforeTheFirstPromptIsNotOutput) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  // The shell cannot reach its first prompt until read receives a newline.
  const auto session = server.new_session(
      {.name = "cold", .shell_command = "read barrier; PS1='' exec sh"});
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;
  const auto tools = all_tools();
  const auto typed = tools.call(server, "paste_text",
                                Arguments{{"paneId", "cold"}, {"text", "echo COLD"}});
  ASSERT_TRUE(typed.has_value()) << typed.error().message;
  const auto waited = tools.call(
      server, "wait_for_text",
      Arguments{{"target", "cold"}, {"text", "COLD"}, {"timeout_ms", "200"}});
  ASSERT_TRUE(waited.has_value()) << waited.error().message;
  EXPECT_FALSE(std::get<bool>(waited->structured.at("matched").value));
  EXPECT_TRUE(std::get<bool>(waited->structured.at("timed_out").value));
  const auto submitted = tools.call(
      server, "paste_text", Arguments{{"paneId", "cold"}, {"text", "\necho COLD\n"}});
  ASSERT_TRUE(submitted.has_value()) << submitted.error().message;
  const auto output = tools.call(
      server, "wait_for_text",
      Arguments{{"target", "cold"}, {"text", "COLD"}, {"timeout_ms", "1000"}});
  ASSERT_TRUE(output.has_value()) << output.error().message;
  EXPECT_TRUE(std::get<bool>(output->structured.at("matched").value));
  EXPECT_TRUE(has_output_line(*output, "COLD")) << string_field(*output, "text");
}

TEST(McpToolsTmux, EchoSubmittedCommandDoesNotHideOutputWithoutANewline) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const auto tools = all_tools();
  const auto typed = tools.call(server, "paste_text",
                                Arguments{{"paneId", fixture->session_name()},
                                          {"text", "printf NO; printf LINE"},
                                          {"enter", "true"}});
  ASSERT_TRUE(typed.has_value()) << typed.error().message;
  const auto waited = tools.call(server, "wait_for_text",
                                 Arguments{{"target", fixture->session_name()},
                                           {"text", "NOLINE"},
                                           {"timeout_ms", "300"}});
  ASSERT_TRUE(waited.has_value()) << waited.error().message;
  EXPECT_TRUE(std::get<bool>(waited->structured.at("matched").value))
      << string_field(*waited, "text");
}

TEST(McpToolsTmux, EchoEditsAreAppliedAndKeyNamesNeverJoinTrackedText) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.new_session({.name = "echo-s4", .shell_command = "sh"});
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;
  auto panes = session->panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  ASSERT_FALSE(panes->empty());
  const std::string pane_id{panes->front().id().value()};
  const auto tools = all_tools();

  const auto typed = tools.call(server, "paste_text",
                                Arguments{{"paneId", pane_id}, {"text", "xMARKER"}});
  ASSERT_TRUE(typed.has_value()) << typed.error().message;
  for (int i = 0; i < 7; ++i) {
    const auto erased = tools.call(server, "send_keys",
                                   Arguments{{"paneId", pane_id}, {"keys", "BSpace"}});
    ASSERT_TRUE(erased.has_value()) << erased.error().message;
  }
  const auto command =
      tools.call(server, "paste_text",
                 Arguments{{"paneId", pane_id}, {"text", "sleep 0.1; echo MARKER"}});
  ASSERT_TRUE(command.has_value()) << command.error().message;
  const auto enter = tools.call(server, "send_keys",
                                Arguments{{"paneId", pane_id}, {"keys", "Enter"}});
  ASSERT_TRUE(enter.has_value()) << enter.error().message;

  const auto waited = tools.call(
      server, "wait_for_text",
      Arguments{{"target", pane_id}, {"text", "MARKER"}, {"timeout_ms", "1000"}});
  ASSERT_TRUE(waited.has_value()) << waited.error().message;
  EXPECT_TRUE(std::get<bool>(waited->structured.at("matched").value))
      << string_field(*waited, "text");
  EXPECT_TRUE(has_output_line(*waited, "MARKER"));
}

TEST(McpToolsTmux, EchoUnknownKeyFailsOpenRatherThanHidingRealOutput) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.new_session({.name = "echo-s6", .shell_command = "sh"});
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;
  auto panes = session->panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  ASSERT_FALSE(panes->empty());
  const std::string pane_id{panes->front().id().value()};
  const auto tools = all_tools();

  const auto split = tools.call(server, "split_window", Arguments{{"paneId", pane_id}});
  ASSERT_TRUE(split.has_value()) << split.error().message;
  const std::string writer_pane_id = string_field(*split, "pane_id");

  const auto typed = tools.call(server, "paste_text",
                                Arguments{{"paneId", pane_id}, {"text", "xMARKER"}});
  ASSERT_TRUE(typed.has_value()) << typed.error().message;
  const auto left =
      tools.call(server, "send_keys", Arguments{{"paneId", pane_id}, {"keys", "Left"}});
  ASSERT_TRUE(left.has_value()) << left.error().message;

  std::thread injector{[&] {
    echo_contract_test::inject_foreign_output(server, tools, writer_pane_id, pane_id,
                                              "xMARKER", 0.3);
  }};
  const auto waited = tools.call(
      server, "wait_for_text",
      Arguments{{"target", pane_id}, {"text", "xMARKER"}, {"timeout_ms", "1000"}});
  injector.join();

  ASSERT_TRUE(waited.has_value()) << waited.error().message;
  EXPECT_TRUE(std::get<bool>(waited->structured.at("matched").value))
      << "the abandoned typing still masked a later, genuine line that "
         "happens to repeat it; text=\""
      << string_field(*waited, "text") << "\"";
}

TEST(McpToolsTmux, EchoResizeControlStillMatchesRealOutputAfterReflow) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.new_session({.name = "echo-resize", .shell_command = "sh"});
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;
  auto panes = session->panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  ASSERT_FALSE(panes->empty());
  const std::string pane_id{panes->front().id().value()};
  const auto tools = all_tools();

  const auto split = tools.call(server, "split_window", Arguments{{"paneId", pane_id}});
  ASSERT_TRUE(split.has_value()) << split.error().message;
  const std::string writer_pane_id = string_field(*split, "pane_id");

  const auto typed =
      tools.call(server, "send_keys", Arguments{{"paneId", pane_id}, {"keys", "y"}});
  ASSERT_TRUE(typed.has_value()) << typed.error().message;

  const auto window = panes->front().window();
  ASSERT_TRUE(window.has_value()) << window.error().diagnostic;
  const auto resized = window->resize(100, 40);
  ASSERT_TRUE(resized.has_value()) << resized.error().diagnostic;

  std::thread injector{[&] {
    echo_contract_test::inject_foreign_output(server, tools, writer_pane_id, pane_id,
                                              "ready", 0.3);
  }};
  const auto waited = tools.call(
      server, "wait_for_text",
      Arguments{{"target", pane_id}, {"text", "ready"}, {"timeout_ms", "1000"}});
  injector.join();

  ASSERT_TRUE(waited.has_value()) << waited.error().message;
  EXPECT_TRUE(std::get<bool>(waited->structured.at("matched").value))
      << "a window resize between send and match hid real output; text=\""
      << string_field(*waited, "text") << "\"";
}

} // namespace
