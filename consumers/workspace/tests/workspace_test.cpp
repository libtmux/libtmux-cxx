#include "libtmux_consumers/workspace.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "libtmux/cardinality.hpp"
#include "support/scoped_tmux_server.hpp"

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

  // Both held back so the text stays on the command line to be read, and
  // one suppressed: the difference is the leading space a shell configured
  // for it uses to keep the line out of history.
  const workspace::Workspace description{
      .session_name = "hidden",
      .windows = {{.name = "work",
                   .panes = {{.shell_commands = {{.text = "echo hidden-marker",
                                                  .enter = false,
                                                  .suppress_history = true}}},
                             {.shell_commands = {{.text = "echo hidden-marker",
                                                  .enter = false,
                                                  .suppress_history = false}}}}}}};
  const auto built = workspace::build(server, description);
  ASSERT_TRUE(built.has_value()) << built.error().reason;

  const auto windows = built->windows();
  ASSERT_TRUE(windows.has_value()) << windows.error().diagnostic;
  const auto panes = windows->front().panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  ASSERT_EQ(panes->size(), 2U);

  // Polled, because a pane draws what was typed into it when its shell gets
  // round to it, and the second one has had less time than the first.
  // Answering with the position rather than the text in front of it: a
  // command at column zero has nothing in front of it, which is not the
  // same as not having been typed.
  const auto column_of = [](const libtmux::Pane& pane) -> std::optional<std::size_t> {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline) {
      const auto shown = pane.capture();
      EXPECT_TRUE(shown.has_value());
      const std::string screen = shown.value_or(std::string{});
      if (const auto at = screen.find("echo hidden-marker"); at != std::string::npos) {
        const auto line = screen.rfind('\n', at);
        return at - (line == std::string::npos ? 0U : line + 1U);
      }
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

} // namespace
