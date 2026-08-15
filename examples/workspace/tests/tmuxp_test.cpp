// Reading a tmuxp document, and building what it describes.

#include "libtmux_consumers/tmuxp.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "libtmux/server.hpp"
#include "support/scoped_tmux_server.hpp"

namespace {

using libtmux::Server;
using libtmux::workspace::parse_tmuxp;

Server connect(const libtmux::test::ScopedTmuxServer& fixture) {
  auto server = Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value());
  return *server;
}

// The shape a tmuxp document actually takes, including both ways of writing
// a pane and both spellings of a directory.
constexpr std::string_view kDocument = R"(
session_name: work
start_directory: /tmp
windows:
  - window_name: editor
    layout: main-vertical
    panes:
      - shell_command:
          - echo first
          - echo second
      - echo lone
  - window_name: logs
    root: /
    panes:
      - shell_command: echo tail
      -
)";

// The text of each command, which is all most of these expectations are
// about now that a command carries how to send it too.
std::vector<std::string>
texts(const std::vector<libtmux::workspace::Command>& commands) {
  std::vector<std::string> spoken;
  spoken.reserve(commands.size());
  for (const auto& command : commands) {
    spoken.push_back(command.text);
  }
  return spoken;
}

TEST(Tmuxp, ADocumentBecomesAWorkspace) {
  const auto workspace = parse_tmuxp(kDocument);
  ASSERT_TRUE(workspace.has_value())
      << workspace.error().where << ": " << workspace.error().reason;

  EXPECT_EQ(workspace->session_name, "work");
  EXPECT_EQ(workspace->start_directory, "/tmp");
  ASSERT_EQ(workspace->windows.size(), 2U);

  const auto& editor = workspace->windows.front();
  EXPECT_EQ(editor.name, "editor");
  EXPECT_EQ(editor.layout, "main-vertical");
  ASSERT_EQ(editor.panes.size(), 2U);
  EXPECT_EQ(texts(editor.panes[0].shell_commands),
            (std::vector<std::string>{"echo first", "echo second"}));
  EXPECT_EQ(texts(editor.panes[1].shell_commands),
            (std::vector<std::string>{"echo lone"}));

  const auto& logs = workspace->windows.back();
  EXPECT_EQ(logs.start_directory, "/");
  ASSERT_EQ(logs.panes.size(), 2U);
  EXPECT_EQ(texts(logs.panes[0].shell_commands),
            (std::vector<std::string>{"echo tail"}));
  // An empty entry is a pane that runs nothing, not a missing pane.
  EXPECT_TRUE(logs.panes[1].shell_commands.empty());
}

TEST(Tmuxp, ADocumentThatCannotBeBuiltSaysWhere) {
  const auto no_session = parse_tmuxp("windows:\n  - window_name: only\n");
  ASSERT_FALSE(no_session.has_value());
  EXPECT_EQ(no_session.error().where, "session_name");

  const auto no_windows = parse_tmuxp("session_name: work\n");
  ASSERT_FALSE(no_windows.has_value());
  EXPECT_EQ(no_windows.error().where, "windows");

  const auto bad_pane =
      parse_tmuxp("session_name: work\nwindows:\n  - panes:\n      - [a, b]\n");
  ASSERT_FALSE(bad_pane.has_value());
  EXPECT_EQ(bad_pane.error().where, "windows[0].panes[0]");

  const auto bad_command =
      parse_tmuxp("session_name: work\nwindows:\n  - panes:\n      - shell_command:\n"
                  "          - [a]\n");
  ASSERT_FALSE(bad_command.has_value());
  EXPECT_EQ(bad_command.error().where, "windows[0].panes[0].shell_command[0]");

  // Malformed YAML is a parse failure with a message, not a thrown exception.
  const auto broken = parse_tmuxp("session_name: [unclosed\n");
  ASSERT_FALSE(broken.has_value());
  EXPECT_FALSE(broken.error().reason.empty());
}

TEST(Tmuxp, AParsedDocumentBuildsOnARealServer) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto described = parse_tmuxp(kDocument);
  ASSERT_TRUE(described.has_value()) << described.error().reason;

  const auto built = libtmux::workspace::build(server, *described);
  ASSERT_TRUE(built.has_value()) << built.error().reason;
  EXPECT_EQ(built->name(), "work");

  const auto windows = built->windows();
  ASSERT_TRUE(windows.has_value()) << windows.error().diagnostic;
  ASSERT_EQ(windows->size(), 2U);
  EXPECT_EQ(windows->front().name(), "editor");
  EXPECT_EQ(windows->front().pane_count(), 2);

  // The directory in the document is where the panes start.
  const auto panes = windows->front().panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  // Resolved, because macOS makes /tmp a symlink to /private/tmp and the pane
  // reports where it really is. The document says /tmp either way.
  EXPECT_EQ(std::filesystem::path{panes->front().path()},
            std::filesystem::canonical("/tmp"));

  const auto logs = windows->back().panes();
  ASSERT_TRUE(logs.has_value()) << logs.error().diagnostic;
  EXPECT_EQ(logs->front().path(), "/");
}

// The shape this repository's own tmuxp file takes: setup commands for every
// pane, a focused window and pane, and a window option the layout reads.
constexpr std::string_view kFullDocument = R"(
session_name: libtmux
start_directory: /tmp
shell_command_before:
  - echo setup
windows:
  - window_name: work
    focus: True
    layout: main-horizontal
    options:
      main-pane-height: 60%
    panes:
      - focus: true
      - echo second
  - window_name: docs
    shell_command_before: echo docs-setup
    panes:
      - echo only
)";

TEST(Tmuxp, EveryKeyTheDocumentCarriesReachesTheWorkspace) {
  const auto workspace = parse_tmuxp(kFullDocument);
  ASSERT_TRUE(workspace.has_value())
      << workspace.error().where << ": " << workspace.error().reason;
  ASSERT_EQ(workspace->windows.size(), 2U);

  const auto& work = workspace->windows.front();
  EXPECT_TRUE(work.focus);
  EXPECT_EQ(work.options, (std::vector<std::pair<std::string, std::string>>{
                              {"main-pane-height", "60%"}}));
  ASSERT_EQ(work.panes.size(), 2U);
  EXPECT_TRUE(work.panes[0].focus);
  EXPECT_FALSE(work.panes[1].focus);

  // The document's setup runs first in every pane, including one that had no
  // command of its own.
  EXPECT_EQ(texts(work.panes[0].shell_commands),
            (std::vector<std::string>{"echo setup"}));
  EXPECT_EQ(texts(work.panes[1].shell_commands),
            (std::vector<std::string>{"echo setup", "echo second"}));
  // A window's own setup runs after the document's and before the pane's.
  EXPECT_EQ(texts(workspace->windows[1].panes[0].shell_commands),
            (std::vector<std::string>{"echo setup", "echo docs-setup", "echo only"}));
}

TEST(Tmuxp, VariablesAndAWindowIndexReachTheWorkspace) {
  // Both were found by running tmuxp's own examples through this parser:
  // `session-environment.yaml` and `window-index.yaml` were refused, and
  // neither could be expressed by the library at the time.
  const auto workspace = parse_tmuxp("session_name: w\n"
                                     "environment:\n"
                                     "  SESSION_VAR: from-session\n"
                                     "windows:\n"
                                     "  - window_name: placed\n"
                                     "    window_index: 4\n"
                                     "    environment:\n"
                                     "      WINDOW_VAR: from-window\n"
                                     "    panes:\n"
                                     "      - shell_command: echo one\n"
                                     "        environment:\n"
                                     "          PANE_VAR: from-pane\n");
  ASSERT_TRUE(workspace.has_value())
      << workspace.error().where << ": " << workspace.error().reason;

  EXPECT_EQ(workspace->environment, (std::vector<std::pair<std::string, std::string>>{
                                        {"SESSION_VAR", "from-session"}}));
  ASSERT_EQ(workspace->windows.size(), 1U);
  const auto& window = workspace->windows.front();
  ASSERT_TRUE(window.index.has_value());
  EXPECT_EQ(*window.index, 4);
  EXPECT_EQ(window.environment, (std::vector<std::pair<std::string, std::string>>{
                                    {"WINDOW_VAR", "from-window"}}));
  ASSERT_EQ(window.panes.size(), 1U);
  EXPECT_EQ(
      window.panes.front().environment,
      (std::vector<std::pair<std::string, std::string>>{{"PANE_VAR", "from-pane"}}));
}

TEST(Tmuxp, AnEnvironmentThatIsNotAMappingIsRefused) {
  const auto listed =
      parse_tmuxp("session_name: w\nenvironment:\n  - A=b\nwindows:\n  - panes: [x]\n");
  ASSERT_FALSE(listed.has_value());
  EXPECT_EQ(listed.error().where, "environment");
  EXPECT_EQ(listed.error().reason, "an environment is a mapping");
}

TEST(Tmuxp, ACommandCanBeAMappingRatherThanAString) {
  // Four of tmuxp's own examples turn on this form alone.
  const auto workspace = parse_tmuxp("session_name: w\n"
                                     "windows:\n"
                                     "  - panes:\n"
                                     "      - shell_command:\n"
                                     "          - echo plain\n"
                                     "          - cmd: echo held\n"
                                     "            enter: false\n"
                                     "          - cmd: echo paused\n"
                                     "            sleep_before: 2\n"
                                     "            sleep_after: 0.5\n");
  ASSERT_TRUE(workspace.has_value())
      << workspace.error().where << ": " << workspace.error().reason;
  const auto& commands = workspace->windows.front().panes.front().shell_commands;
  ASSERT_EQ(commands.size(), 3U);

  // A string is a command that is sent and submitted, with no waiting.
  EXPECT_EQ(commands[0], (libtmux::workspace::Command{.text = "echo plain"}));
  // `enter: false` keeps the text on the command line.
  EXPECT_EQ(commands[1],
            (libtmux::workspace::Command{.text = "echo held", .enter = false}));
  // Seconds in the document, and a fraction of one is a real value.
  EXPECT_EQ(commands[2], (libtmux::workspace::Command{
                             .text = "echo paused",
                             .pause_before = std::chrono::seconds{2},
                             .pause_after = std::chrono::milliseconds{500}}));
}

TEST(Tmuxp, ABlankCommandIsACarriageReturnRatherThanNothing) {
  // tmuxp's `blank-panes.yaml` writes a pane four ways that all mean "open
  // it and run nothing".
  const auto workspace = parse_tmuxp("session_name: w\n"
                                     "windows:\n"
                                     "  - panes:\n"
                                     "      -\n"
                                     "      - \"\"\n"
                                     "      - shell_command:\n");
  ASSERT_TRUE(workspace.has_value())
      << workspace.error().where << ": " << workspace.error().reason;
  const auto& panes = workspace->windows.front().panes;
  ASSERT_EQ(panes.size(), 3U);
  EXPECT_TRUE(panes[0].shell_commands.empty());
  EXPECT_EQ(panes[1].shell_commands, (std::vector<libtmux::workspace::Command>{{}}));
  EXPECT_TRUE(panes[2].shell_commands.empty());
}

TEST(Tmuxp, AMalformedCommandMappingIsRefused) {
  const auto unknown =
      parse_tmuxp("session_name: w\nwindows:\n  - panes:\n"
                  "      - shell_command:\n          - cmd: x\n            wat: 1\n");
  ASSERT_FALSE(unknown.has_value());
  EXPECT_EQ(unknown.error().reason, "unsupported key: wat");

  const auto backwards = parse_tmuxp(
      "session_name: w\nwindows:\n  - panes:\n"
      "      - shell_command:\n          - cmd: x\n            sleep_before: -1\n");
  ASSERT_FALSE(backwards.has_value());
  EXPECT_EQ(backwards.error().reason, "a pause cannot be negative");

  const auto listed =
      parse_tmuxp("session_name: w\nwindows:\n  - panes:\n"
                  "      - shell_command:\n          - [not, a, command]\n");
  ASSERT_FALSE(listed.has_value());
  EXPECT_EQ(listed.error().reason, "a command is a string or a mapping");
}

TEST(Tmuxp, TheKeysThatTookTheCorpusToTwentyOne) {
  // Each of these was a refusal in tmuxp's own examples. Read together
  // because a document uses them together.
  const auto workspace = parse_tmuxp("session_name: w\n"
                                     "global_options:\n"
                                     "  default-shell: /bin/sh\n"
                                     "options:\n"
                                     "  main-pane-height: 30\n"
                                     "windows:\n"
                                     "  - window_name: one\n"
                                     "    window_shell: /usr/bin/python3\n"
                                     "    suppress_history: false\n"
                                     "    options_after:\n"
                                     "      synchronize-panes: on\n"
                                     "    panes:\n"
                                     "      - shell: /usr/bin/vim\n"
                                     "        sleep_before: 1\n"
                                     "        enter: false\n"
                                     "        shell_command:\n"
                                     "          - echo one\n"
                                     "          - echo two\n");
  ASSERT_TRUE(workspace.has_value())
      << workspace.error().where << ": " << workspace.error().reason;

  EXPECT_EQ(
      workspace->global_options,
      (std::vector<std::pair<std::string, std::string>>{{"default-shell", "/bin/sh"}}));
  EXPECT_EQ(workspace->options, (std::vector<std::pair<std::string, std::string>>{
                                    {"main-pane-height", "30"}}));
  const auto& window = workspace->windows.front();
  EXPECT_EQ(window.shell, "/usr/bin/python3");
  EXPECT_EQ(window.options_after, (std::vector<std::pair<std::string, std::string>>{
                                      {"synchronize-panes", "on"}}));
  const auto& pane = window.panes.front();
  EXPECT_EQ(pane.shell, "/usr/bin/vim");

  // A pane sets for all its commands what a command can set for itself.
  ASSERT_EQ(pane.shell_commands.size(), 2U);
  for (const auto& command : pane.shell_commands) {
    EXPECT_FALSE(command.enter);
    EXPECT_FALSE(command.suppress_history);
    EXPECT_EQ(command.pause_before, std::chrono::seconds{1});
  }
}

TEST(Tmuxp, AKeyThisCannotHonourIsRefusedRatherThanDropped) {
  // Silently ignoring one of these builds a workspace that is missing part of
  // what was asked for, with nothing in the result to say so.
  // `before_script` runs a program before the workspace is built, which is
  // the caller's to do; nothing here can honour it.
  const auto document = parse_tmuxp(
      "session_name: w\nbefore_script: ./setup.sh\nwindows:\n  - panes: [x]\n");
  ASSERT_FALSE(document.has_value());
  EXPECT_EQ(document.error().where, "");
  EXPECT_EQ(document.error().reason, "unsupported key: before_script");

  const auto window = parse_tmuxp(
      "session_name: w\nwindows:\n  - not_a_window_key: 1\n    panes: [x]\n");
  ASSERT_FALSE(window.has_value());
  EXPECT_EQ(window.error().where, "windows[0]");
  EXPECT_EQ(window.error().reason, "unsupported key: not_a_window_key");

  const auto pane = parse_tmuxp("session_name: w\nwindows:\n  - panes:\n"
                                "      - shell_command: x\n        wat: false\n");
  ASSERT_FALSE(pane.has_value());
  EXPECT_EQ(pane.error().where, "windows[0].panes[0]");
  EXPECT_EQ(pane.error().reason, "unsupported key: wat");

  const auto options =
      parse_tmuxp("session_name: w\nwindows:\n  - options: [a]\n    panes: [x]\n");
  ASSERT_FALSE(options.has_value());
  EXPECT_EQ(options.error().where, "windows[0].options");
}

TEST(Tmuxp, FocusAndOptionsLandOnTheBuiltSession) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto described = parse_tmuxp(kFullDocument);
  ASSERT_TRUE(described.has_value()) << described.error().reason;
  const auto built = libtmux::workspace::build(server, *described);
  ASSERT_TRUE(built.has_value()) << built.error().reason;

  const auto windows = built->windows();
  ASSERT_TRUE(windows.has_value()) << windows.error().diagnostic;
  ASSERT_EQ(windows->size(), 2U);

  // The focused window is the active one, even though the second window was
  // built later and its panes were addressed after.
  EXPECT_TRUE(windows->front().active());
  EXPECT_FALSE(windows->back().active());

  // The option reached tmux, and reached it before the layout was applied.
  const auto height = windows->front().option("main-pane-height");
  ASSERT_TRUE(height.has_value()) << height.error().diagnostic;
  EXPECT_EQ(height->value, "60%");

  const auto panes = windows->front().panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  ASSERT_EQ(panes->size(), 2U);
  EXPECT_TRUE(panes->front().active());
}

} // namespace
