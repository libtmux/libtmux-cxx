#include "libtmux_consumers/workspace.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "libtmux/cardinality.hpp"
#include "libtmux/testing/scoped_server.hpp"

namespace {

using libtmux::first;
using libtmux::matching;
using libtmux::Server;
namespace window = libtmux::window;
namespace workspace = libtmux::workspace;

Server connect(const libtmux::test::ScopedTmuxServer& fixture) {
  auto server = Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value());
  return server.value();
}

TEST(WorkspaceBuilder, BuildsEveryWindowAndPaneDescribed) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const workspace::Workspace description{
      .session_name = "built",
      .windows = {{.name = "editor", .panes = {{}, {}}},
                  {.name = "logs", .panes = {{}}}}};
  const auto built = workspace::build(server, description);
  ASSERT_TRUE(built.has_value()) << built.error().reason;
  EXPECT_EQ(built->name(), "built");

  const auto windows = server.windows();
  ASSERT_TRUE(windows.has_value()) << windows.error().diagnostic;
  auto editors = *windows | matching(window::name == "editor");
  auto logs = *windows | matching(window::name == "logs");
  EXPECT_TRUE(first(editors).has_value());
  EXPECT_TRUE(first(logs).has_value());

  const auto panes = server.panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  // Two panes in the editor window, one in logs, plus the fixture's session.
  EXPECT_GE(panes->size(), 3U);
}

TEST(WorkspaceBuilder, RunsEachPaneCommandInThePaneItDescribed) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  // A base index the built session must not assume away.
  ASSERT_TRUE(server.run({"set-option", "-g", "pane-base-index", "1"}).has_value());

  // Quoted, so the shell's echo of the command differs from what running it
  // produces: the assertion below cannot be satisfied by the echo alone.
  const workspace::Workspace description{
      .session_name = "indexed",
      .windows = {{.name = "work",
                   .panes = {{.shell_commands = {{.text = "echo first''-pane"}}},
                             {.shell_commands = {{.text = "echo second''-pane"}}}}}}};
  const auto built = workspace::build(server, description);
  ASSERT_TRUE(built.has_value()) << built.error().reason;

  const auto windows = built->windows();
  ASSERT_TRUE(windows.has_value()) << windows.error().diagnostic;
  ASSERT_EQ(windows->size(), 1U);
  const auto panes = windows->front().panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  ASSERT_EQ(panes->size(), 2U);

  for (const auto& [pane, marker] : {std::pair{&panes->front(), "first-pane"},
                                     std::pair{&panes->back(), "second-pane"}}) {
    std::string captured;
    for (int attempt = 0; attempt < 200; ++attempt) {
      const auto text = pane->capture();
      ASSERT_TRUE(text.has_value()) << text.error().diagnostic;
      captured = *text;
      if (captured.find(marker) != std::string::npos) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }
    EXPECT_NE(captured.find(marker), std::string::npos) << marker;
  }
}

TEST(WorkspaceBuilder, VariablesAndAnIndexReachTheRunningServer) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  auto server = Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());

  // Parsing a document is not the same claim as tmux receiving what it said.
  libtmux::workspace::Workspace description;
  description.session_name = "carried";
  description.environment = {{"WS_SESSION", "yes"}};
  libtmux::workspace::Window placed;
  placed.name = "placed";
  placed.index = 6;
  placed.environment = {{"WS_WINDOW", "yes"}};
  description.windows = {libtmux::workspace::Window{.name = "first"}, placed};

  const auto built = libtmux::workspace::build(*server, description);
  ASSERT_TRUE(built.has_value()) << built.error().reason;

  const auto windows = built->windows();
  ASSERT_TRUE(windows.has_value()) << windows.error().diagnostic;
  const auto found = std::ranges::find_if(
      *windows, [](const libtmux::Window& w) { return w.name() == "placed"; });
  ASSERT_NE(found, windows->end());
  EXPECT_EQ(found->index(), 6);

  // The variables are on the processes, which is the only claim that counts.
  const auto shown = found->expand("#{pane_id}");
  ASSERT_TRUE(shown.has_value()) << shown.error().diagnostic;
  const auto environment = server->run({"show-environment", "-t", "carried"});
  ASSERT_TRUE(environment.has_value()) << environment.error().diagnostic;
  EXPECT_NE(environment->find("WS_SESSION=yes"), std::string::npos) << *environment;
}

TEST(WorkspaceBuilder, ACommandHeldBackIsTypedButNotRun) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  // Two commands: one run, one left on the command line. The difference is
  // the whole point of `enter: false`, and capturing the pane is the only
  // way to see it — both were dispatched either way.
  const workspace::Workspace description{
      .session_name = "held",
      .windows = {{.name = "work",
                   .panes = {{.shell_commands = {
                                  {.text = "echo RAN''-IT"},
                                  {.text = "echo HELD''-BACK", .enter = false}}}}}}};
  const auto built = workspace::build(server, description);
  ASSERT_TRUE(built.has_value()) << built.error().reason;

  const auto windows = built->windows();
  ASSERT_TRUE(windows.has_value()) << windows.error().diagnostic;
  const auto panes = windows->front().panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  ASSERT_FALSE(panes->empty());

  std::string screen;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (std::chrono::steady_clock::now() < deadline) {
    const auto shown = panes->front().capture();
    ASSERT_TRUE(shown.has_value()) << shown.error().diagnostic;
    screen = *shown;
    if (screen.find("RAN-IT") != std::string::npos) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
  }

  // The first ran, so its output is on the screen without the quotes the
  // command carried. The second is only there as typed.
  EXPECT_NE(screen.find("RAN-IT"), std::string::npos) << screen;
  EXPECT_NE(screen.find("echo HELD''-BACK"), std::string::npos) << screen;
  EXPECT_EQ(screen.find("HELD-BACK"), std::string::npos) << screen;
}

TEST(WorkspaceBuilder, EachPauseIsWaitedOutRatherThanRecordedAndIgnored) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  // Measured against a build of the same shape carrying no pause. An
  // absolute duration would pass whenever the machine was slow enough; a
  // difference holds under load, because load inflates both. Each pause is
  // measured on its own, so a failure says which one was not taken.
  constexpr auto kPause = std::chrono::milliseconds{700};
  const auto measure = [&server](std::string name, std::chrono::milliseconds before,
                                 std::chrono::milliseconds after) {
    const workspace::Workspace description{
        .session_name = std::move(name),
        .windows = {{.name = "work",
                     .panes = {{.shell_commands = {{.text = "true",
                                                    .pause_before = before,
                                                    .pause_after = after}}}}}}};
    const auto started = std::chrono::steady_clock::now();
    const auto built = workspace::build(server, description);
    EXPECT_TRUE(built.has_value());
    return std::chrono::steady_clock::now() - started;
  };
  const auto ms = [](auto duration) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration);
  };
  constexpr auto kNone = std::chrono::milliseconds{0};

  const auto prompt = measure("prompt", kNone, kNone);
  const auto waits_before = measure("before", kPause, kNone);
  const auto waits_after = measure("after", kNone, kPause);

  EXPECT_GE(waits_before - prompt, kPause / 2)
      << "prompt=" << ms(prompt) << " before=" << ms(waits_before);
  EXPECT_GE(waits_after - prompt, kPause / 2)
      << "prompt=" << ms(prompt) << " after=" << ms(waits_after);
}

TEST(WorkspaceBuilder, OptionsReachTheServerTheSessionAndTheWindow) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  // `synchronize-panes` is the reason the two window lists are separate: set
  // before the splits it types into panes still being made, so it is asked
  // for after them and has to still be there.
  const workspace::Workspace description{
      .session_name = "optioned",
      .global_options = {{"display-time", "1234"}},
      .options = {{"base-index", "3"}},
      .windows = {{.name = "work",
                   .options = {{"main-pane-height", "7"}},
                   .options_after = {{"synchronize-panes", "on"}},
                   .panes = {{}, {}}}}};
  const auto built = workspace::build(server, description);
  ASSERT_TRUE(built.has_value()) << built.error().reason;

  const auto global = server.global_options();
  ASSERT_TRUE(global.has_value()) << global.error().diagnostic;
  EXPECT_TRUE(std::ranges::any_of(*global, [](const libtmux::OptionEntry& entry) {
    return entry.name == "display-time" && entry.value == "1234";
  }));

  const auto session_option = built->option("base-index");
  ASSERT_TRUE(session_option.has_value()) << session_option.error().diagnostic;
  EXPECT_EQ(session_option->value, "3");

  const auto windows = built->windows();
  ASSERT_TRUE(windows.has_value()) << windows.error().diagnostic;
  const auto after = windows->front().option("synchronize-panes");
  ASSERT_TRUE(after.has_value()) << after.error().diagnostic;
  EXPECT_EQ(after->value, "on");
  const auto before = windows->front().option("main-pane-height");
  ASSERT_TRUE(before.has_value()) << before.error().diagnostic;
  EXPECT_EQ(before->value, "7");
}

TEST(WorkspaceBuilder, ASuppressedCommandIsTypedWithTheSpaceThatHidesIt) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  // Both held back in quiet readers so a shell prompt cannot race the text
  // being measured. One is suppressed: the exact leading space is what a
  // configured shell uses to keep the line out of history.
  const workspace::Workspace description{
      .session_name = "hidden",
      .windows = {{.name = "work",
                   .panes = {{.shell_commands = {{.text = "echo hidden-marker",
                                                  .enter = false,
                                                  .suppress_history = true}},
                              .shell = "cat"},
                             {.shell_commands = {{.text = "echo hidden-marker",
                                                  .enter = false,
                                                  .suppress_history = false}},
                              .shell = "cat"}}}}};
  const auto built = workspace::build(server, description);
  ASSERT_TRUE(built.has_value()) << built.error().reason;

  const auto windows = built->windows();
  ASSERT_TRUE(windows.has_value()) << windows.error().diagnostic;
  const auto panes = windows->front().panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  ASSERT_EQ(panes->size(), 2U);

  // Polled, because a pane may draw what was typed after the build returns,
  // and the second one has had less time than the first.
  // Answering with the position rather than the text in front of it: a
  // command at column zero has nothing in front of it, which is not the
  // same as not having been typed.
  //
  // Held until two readings agree. A single capture can catch the line half
  // rendered and answer with a column the terminal is about to move.
  const auto column_of = [](const libtmux::Pane& pane) -> std::optional<std::size_t> {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    const auto reading = [&pane]() -> std::optional<std::size_t> {
      const auto shown = pane.capture();
      EXPECT_TRUE(shown.has_value());
      const std::string screen = shown.value_or(std::string{});
      const auto at = screen.find("echo hidden-marker");
      if (at == std::string::npos) {
        return std::nullopt;
      }
      const auto line = screen.rfind('\n', at);
      return at - (line == std::string::npos ? 0U : line + 1U);
    };
    std::optional<std::size_t> settled;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto seen = reading();
      if (seen.has_value() && settled == seen) {
        return seen;
      }
      settled = seen;
      std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return std::nullopt;
  };

  const auto suppressed = column_of(panes->front());
  const auto plain = column_of(panes->back());
  ASSERT_TRUE(suppressed.has_value());
  ASSERT_TRUE(plain.has_value());
  // One column further in, which is the space that hides it.
  EXPECT_EQ(*suppressed, *plain + 1U);
}

TEST(WorkspaceBuilder, RefusesASessionNameThatCannotAddressItself) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const workspace::Workspace description{.session_name = "a:b",
                                         .windows = {{.name = "w"}}};
  const auto built = workspace::build(server, description);
  ASSERT_FALSE(built.has_value());
  EXPECT_NE(built.error().reason.find("address itself"), std::string::npos);
}

TEST(WorkspaceBuilder, RefusesAnEmptyDescription) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  EXPECT_FALSE(workspace::build(server, {}).has_value());
}

TEST(WorkspaceBuilder, CreatedIdentitiesSurviveDuplicateNamesAndIndexes) {
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("ws")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const workspace::Workspace description{
      .session_name = "identity",
      .windows = {
          {.name = "same",
           .index = 6,
           .panes = {{.shell_commands = {{.text = "FIRST", .enter = false}}}}},
          {.name = "same",
           .index = 2,
           .panes = {{.shell_commands = {{.text = "SECOND", .enter = false}}}}}}};
  const auto built = workspace::build(server, description);
  ASSERT_TRUE(built.has_value()) << built.error().reason;
  const auto windows = built->windows();
  ASSERT_TRUE(windows.has_value());
  ASSERT_EQ(windows->size(), 2U);
  for (const auto& [index, marker] :
       {std::pair{2LL, "SECOND"}, std::pair{6LL, "FIRST"}}) {
    const auto found = std::ranges::find_if(
        *windows, [index](const auto& item) { return item.index() == index; });
    ASSERT_NE(found, windows->end());
    const auto pane = found->active_pane();
    ASSERT_TRUE(pane.has_value());
    const auto captured = pane->capture();
    ASSERT_TRUE(captured.has_value());
    EXPECT_NE(captured->find(marker), std::string::npos) << *captured;
  }
}

TEST(WorkspaceBuilder, FailureRemovesOnlyTheSessionItCreated) {
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("ws")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  // A malformed layout crashes tmux 3.3a instead of returning a command error.
  const workspace::Workspace description{
      .session_name = "failure",
      .windows = {{.name = "bad", .options = {{"not-a-window-option", "on"}}}}};
  EXPECT_FALSE(workspace::build(server, description).has_value());
  EXPECT_FALSE(server.session("failure").has_value());
  EXPECT_TRUE(server.session("libtmux_test").has_value());
  auto borrowed = description;
  borrowed.session_name = "libtmux_test";
  EXPECT_FALSE(workspace::build(server, borrowed).has_value());
  EXPECT_TRUE(server.session("libtmux_test").has_value());
}

TEST(WorkspaceBuilder, InvalidLayoutsPrecedeBeforeBuildCallbacks) {
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("layout")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  for (const auto* layout : {"invalid-layout", "32d2,80x24,0,0{}"}) {
    SCOPED_TRACE(layout);
    bool called = false;
    const workspace::Workspace description{
        .session_name = "invalid", .windows = {{.name = "main", .layout = layout}}};
    const auto built =
        workspace::build(server, description,
                         [&](const libtmux::Session&) -> std::optional<std::string> {
                           called = true;
                           return std::nullopt;
                         });
    EXPECT_FALSE(built.has_value());
    EXPECT_FALSE(called);
    EXPECT_FALSE(server.session("invalid").has_value());
    EXPECT_TRUE(server.session("libtmux_test").has_value());
  }
}

TEST(WorkspaceBuilder, SavedLayoutsAllowTmuxPruningAndSizing) {
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("layout")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  for (const auto* layout :
       {"203f,80x24,0,0{39x24,0,0,40x24,40,0}",
        "8a08,1x1,0,0{39x24,0,0,0,40x24,40,0,1}",
        "79f5,80x24,0,0{39x23,0,0,0,40x24,40,0,1}",
        "ac5c,80x24,0,0{39x24,0,0,0,40x24,40,0[40x11,40,0,1,40x12,40,12,2]}"}) {
    SCOPED_TRACE(layout);
    const workspace::Workspace description{
        .session_name = "saved", .windows = {{.name = "main", .layout = layout}}};
    const auto built = workspace::build(server, description);
    ASSERT_TRUE(built.has_value()) << built.error().reason;
    const auto panes = built->panes();
    ASSERT_TRUE(panes.has_value());
    EXPECT_EQ(panes->size(), 1U);
    ASSERT_TRUE(built->kill().has_value());
    EXPECT_TRUE(server.session("libtmux_test").has_value());
  }
}

TEST(WorkspaceBuilder, NamedLayoutsFollowTheRunningDaemonVersion) {
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("layout")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const auto reply = server.run({"display-message", "-p", "#{version}"});
  ASSERT_TRUE(reply.has_value());
  const auto version = libtmux::parse_version("tmux " + *reply);
  ASSERT_TRUE(version.has_value());
  const bool mirrored = *version >= libtmux::Version{.major = 3, .minor = 5};
  for (const auto& [layout, valid] :
       {std::pair{"main-h", !mirrored}, std::pair{"main-horizontal-mirrored", mirrored},
        std::pair{"even-h", true}}) {
    SCOPED_TRACE(layout);
    bool called = false;
    const workspace::Workspace description{
        .session_name = "named", .windows = {{.name = "main", .layout = layout}}};
    const auto built =
        workspace::build(server, description,
                         [&](const libtmux::Session&) -> std::optional<std::string> {
                           called = true;
                           return std::nullopt;
                         });
    EXPECT_EQ(built.has_value(), valid);
    EXPECT_EQ(called, valid);
    if (built)
      ASSERT_TRUE(built->kill().has_value());
    EXPECT_TRUE(server.session("libtmux_test").has_value());
  }
}

TEST(WorkspaceBuilder, DeferredOptionsDoNotBroadcastStartupCommands) {
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("ws")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const workspace::Workspace description{
      .session_name = "deferred",
      .windows = {{.name = "work",
                   .options_after = {{"synchronize-panes", "on"}},
                   .panes = {{.shell_commands = {{.text = "ALPHA", .enter = false}}},
                             {.shell_commands = {{.text = "BETA", .enter = false}}}}}}};
  const auto built = workspace::build(server, description);
  ASSERT_TRUE(built.has_value()) << built.error().reason;
  const auto windows = built->windows();
  ASSERT_TRUE(windows.has_value());
  const auto panes = windows->front().panes();
  ASSERT_TRUE(panes.has_value());
  ASSERT_EQ(panes->size(), 2U);
  const auto first = panes->front().capture();
  const auto second = panes->back().capture();
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_NE(first->find("ALPHA"), std::string::npos);
  EXPECT_EQ(first->find("BETA"), std::string::npos);
  EXPECT_NE(second->find("BETA"), std::string::npos);
  EXPECT_EQ(second->find("ALPHA"), std::string::npos);
}

TEST(WorkspaceBuilder, EveryPaneReceivesItsLauncherAndEnvironment) {
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("ws")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const workspace::Command command{
      .text =
          "printf '%s:%s:%s\\n' \"$WS_SESSION\" \"${WS_WINDOW-unset}\" \"$WS_LAUNCH\""};
  const workspace::Workspace description{
      .session_name = "environment",
      .environment = {{"WS_SESSION", "session"}},
      .windows = {
          {.name = "work",
           .shell = "/bin/sh -c 'export WS_LAUNCH=launcher; exec /bin/sh'",
           .environment = {{"WS_WINDOW", "window"}},
           .panes = {{.shell_commands = {command}},
                     {.shell_commands = {command}, .environment_overrides = true}}}}};
  const auto built = workspace::build(server, description);
  ASSERT_TRUE(built.has_value()) << built.error().reason;
  const auto windows = built->windows();
  ASSERT_TRUE(windows.has_value());
  const auto panes = windows->front().panes();
  ASSERT_TRUE(panes.has_value());
  ASSERT_EQ(panes->size(), 2U);
  for (const auto& [pane, marker] :
       {std::pair{&panes->front(), "session:window:launcher"},
        std::pair{&panes->back(), "session:unset:launcher"}}) {
    std::string captured;
    for (int attempt = 0; attempt < 20; ++attempt) {
      const auto output = pane->capture();
      ASSERT_TRUE(output.has_value());
      captured = *output;
      if (captured.find(marker) != std::string::npos) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    EXPECT_NE(captured.find(marker), std::string::npos) << captured;
  }
}

} // namespace
