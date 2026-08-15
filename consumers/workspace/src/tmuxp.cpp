#include "libtmux_consumers/tmuxp.hpp"
#include "libtmux/expected.hpp"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <yaml-cpp/yaml.h>

namespace libtmux::workspace {

namespace {

libtmux::unexpected_t<ParseError> fail(std::string where, std::string reason) {
  return libtmux::unexpected(ParseError{std::move(where), std::move(reason)});
}

// Every key this consumer acts on. A document may carry more — tmuxp's own
// schema is much larger — and the one thing that must not happen is reading
// such a document as if it said less: a dropped `shell_command_before` builds
// a workspace whose panes never activate their environment, and nothing about
// the result says so. Refusing names the key instead.
std::optional<std::string> unknown_key(const YAML::Node& node,
                                       std::span<const std::string_view> keys) {
  for (const auto& entry : node) {
    if (!entry.first.IsScalar()) {
      return "a key is not a name";
    }
    const auto key = entry.first.as<std::string>();
    if (std::ranges::find(keys, key) == keys.end()) {
      return key;
    }
  }
  return std::nullopt;
}

bool is_true(const YAML::Node& node) {
  if (!node || !node.IsScalar()) {
    return false;
  }
  // tmuxp documents in the wild write this as `true`, `True` and `yes`, and
  // yaml-cpp reads all three as a bool.
  bool flag = false;
  return YAML::convert<bool>::decode(node, flag) && flag;
}

// A command is written as a string or as a mapping carrying `cmd` and how
// to send it. Both forms appear in the same list in tmuxp's own examples.
libtmux::expected<Command, ParseError> read_command(const YAML::Node& node,
                                                    const std::string& where) {
  if (!node || node.IsNull()) {
    // A blank entry opens a pane and runs nothing in it.
    return Command{};
  }
  if (node.IsScalar()) {
    return Command{.text = node.as<std::string>()};
  }
  if (!node.IsMap()) {
    return fail(where, "a command is a string or a mapping");
  }
  static constexpr std::string_view kCommandKeys[]{"cmd", "enter", "sleep_before",
                                                   "sleep_after", "suppress_history"};
  // A pane is read through here too, for the defaults it sets for its own
  // commands, and it carries keys of its own that were checked there.
  static constexpr std::string_view kPaneOnlyKeys[]{
      "shell_command", "start_directory", "root", "focus", "environment", "shell"};
  if (const auto unknown = unknown_key(node, kCommandKeys)) {
    const bool from_a_pane =
        std::ranges::find(kPaneOnlyKeys, *unknown) != std::ranges::end(kPaneOnlyKeys);
    if (!from_a_pane) {
      return fail(where, "unsupported key: " + *unknown);
    }
  }
  Command command;
  if (const YAML::Node text = node["cmd"]; text && text.IsScalar()) {
    command.text = text.as<std::string>();
  }
  // Absent means send it; only an explicit false holds the text back.
  if (const YAML::Node enter = node["enter"]; enter && enter.IsScalar()) {
    command.enter = is_true(enter);
  }
  for (const auto& [key, slot] : {std::pair{"sleep_before", &Command::pause_before},
                                  std::pair{"sleep_after", &Command::pause_after}}) {
    const YAML::Node pause = node[key];
    if (!pause || !pause.IsScalar()) {
      continue;
    }
    try {
      // Seconds in the document, and fractions of one are written there too.
      const auto seconds = pause.as<double>();
      if (seconds < 0.0) {
        return fail(where + "." + key, "a pause cannot be negative");
      }
      command.*slot = std::chrono::milliseconds{static_cast<long long>(seconds * 1000)};
    } catch (const YAML::Exception&) {
      return fail(where + "." + key, "a pause is a number of seconds");
    }
  }
  return command;
}

// A command list is written as one command or as several.
libtmux::expected<std::vector<Command>, ParseError>
read_commands(const YAML::Node& node, const std::string& where) {
  std::vector<Command> commands;
  if (!node || node.IsNull()) {
    return commands;
  }
  if (!node.IsSequence()) {
    auto one = read_command(node, where);
    if (!one.has_value()) {
      return libtmux::unexpected(one.error());
    }
    commands.push_back(*std::move(one));
    return commands;
  }
  for (std::size_t index = 0; index < node.size(); ++index) {
    auto one = read_command(node[index], where + "[" + std::to_string(index) + "]");
    if (!one.has_value()) {
      return libtmux::unexpected(one.error());
    }
    commands.push_back(*std::move(one));
  }
  return commands;
}

// tmuxp writes a directory under either spelling, and has for long enough
// that documents in the wild use both.
std::string directory_of(const YAML::Node& node) {
  for (const char* key : {"start_directory", "root"}) {
    if (const YAML::Node value = node[key]; value && value.IsScalar()) {
      return value.as<std::string>();
    }
  }
  return {};
}

// A pane is a command, a mapping carrying one or several, or nothing at all.
// Options and variables are both mappings of names to values, and tmux takes
// both as ordered commands, so the order the document wrote them in is kept.
libtmux::expected<std::vector<std::pair<std::string, std::string>>, ParseError>
read_pairs(const YAML::Node& node, const std::string& where, const char* what) {
  std::vector<std::pair<std::string, std::string>> pairs;
  if (!node) {
    return pairs;
  }
  if (!node.IsMap()) {
    return fail(where, std::string{what} + " are a mapping");
  }
  for (const auto& entry : node) {
    if (!entry.first.IsScalar() || !entry.second.IsScalar()) {
      return fail(where, std::string{what} + " are names and values");
    }
    pairs.emplace_back(entry.first.as<std::string>(), entry.second.as<std::string>());
  }
  return pairs;
}

// `environment:` is a mapping of names to values, at any of the three levels
// tmux itself accepts one.
libtmux::expected<std::vector<std::pair<std::string, std::string>>, ParseError>
read_environment(const YAML::Node& node, const std::string& where) {
  std::vector<std::pair<std::string, std::string>> variables;
  if (!node) {
    return variables;
  }
  if (!node.IsMap()) {
    return fail(where, "an environment is a mapping");
  }
  for (const auto& entry : node) {
    if (!entry.first.IsScalar() || !entry.second.IsScalar()) {
      return fail(where, "a variable is a name and a value");
    }
    variables.emplace_back(entry.first.as<std::string>(),
                           entry.second.as<std::string>());
  }
  return variables;
}

libtmux::expected<Pane, ParseError> read_pane(const YAML::Node& node,
                                              const std::string& where) {
  Pane pane;
  if (!node || node.IsNull()) {
    return pane;
  }
  if (node.IsScalar()) {
    // A pane written as a bare string is that one command. An empty one is
    // a carriage return, which the builder sends rather than skips.
    pane.shell_commands.push_back(Command{.text = node.as<std::string>()});
    return pane;
  }
  if (!node.IsMap()) {
    return fail(where, "a pane is a command or a mapping");
  }
  static constexpr std::string_view kPaneKeys[]{
      "shell_command", "start_directory", "root",
      "focus",         "environment",     "enter",
      "sleep_before",  "sleep_after",     "suppress_history",
      "shell"};
  if (const auto unknown = unknown_key(node, kPaneKeys)) {
    return fail(where, "unsupported key: " + *unknown);
  }

  pane.start_directory = directory_of(node);
  pane.focus = is_true(node["focus"]);
  auto commands = read_commands(node["shell_command"], where + ".shell_command");
  if (!commands.has_value()) {
    return libtmux::unexpected(commands.error());
  }
  pane.shell_commands = *std::move(commands);
  auto variables = read_environment(node["environment"], where + ".environment");
  if (!variables.has_value()) {
    return libtmux::unexpected(variables.error());
  }
  pane.environment = *std::move(variables);
  if (const YAML::Node shell = node["shell"]; shell && shell.IsScalar()) {
    pane.shell = shell.as<std::string>();
  }

  // A pane may set for all its commands what a command can set for itself.
  // Applied here rather than carried, so the builder has one place to look.
  const YAML::Node enter = node["enter"];
  const bool holds_back = enter && enter.IsScalar() && !is_true(enter);
  const YAML::Node suppress = node["suppress_history"];
  const bool keep_history = suppress && suppress.IsScalar() && !is_true(suppress);
  for (const char* key : {"sleep_before", "sleep_after"}) {
    const YAML::Node pause = node[key];
    if (pause && !pause.IsScalar()) {
      return fail(std::string{where} + "." + key, "a pause is a number of seconds");
    }
  }
  auto shared = read_command(node, where);
  if (!shared.has_value()) {
    return libtmux::unexpected(shared.error());
  }
  for (Command& command : pane.shell_commands) {
    if (holds_back) {
      command.enter = false;
    }
    if (keep_history) {
      command.suppress_history = false;
    }
    if (command.pause_before.count() == 0) {
      command.pause_before = shared->pause_before;
    }
    if (command.pause_after.count() == 0) {
      command.pause_after = shared->pause_after;
    }
  }
  return pane;
}

void prepend_to_each(std::vector<Pane>& panes, const std::vector<Command>& commands) {
  if (commands.empty()) {
    return;
  }
  for (Pane& pane : panes) {
    pane.shell_commands.insert(pane.shell_commands.begin(), commands.begin(),
                               commands.end());
  }
}

libtmux::expected<Window, ParseError> read_window(const YAML::Node& node,
                                                  const std::string& where) {
  if (!node.IsMap()) {
    return fail(where, "a window is a mapping");
  }
  static constexpr std::string_view kWindowKeys[]{
      "window_name",     "layout",       "start_directory",      "root",
      "options",         "focus",        "shell_command_before", "panes",
      "environment",     "window_index", "options_after",        "window_shell",
      "suppress_history"};
  if (const auto unknown = unknown_key(node, kWindowKeys)) {
    return fail(where, "unsupported key: " + *unknown);
  }
  Window window;
  if (const YAML::Node name = node["window_name"]; name && name.IsScalar()) {
    window.name = name.as<std::string>();
  }
  if (const YAML::Node layout = node["layout"]; layout && layout.IsScalar()) {
    window.layout = layout.as<std::string>();
  }
  window.start_directory = directory_of(node);
  window.focus = is_true(node["focus"]);
  if (const YAML::Node index = node["window_index"]; index && index.IsScalar()) {
    try {
      window.index = index.as<long long>();
    } catch (const YAML::Exception&) {
      return fail(where + ".window_index", "a window index is a number");
    }
  }
  auto window_variables = read_environment(node["environment"], where + ".environment");
  if (!window_variables.has_value()) {
    return libtmux::unexpected(window_variables.error());
  }
  window.environment = *std::move(window_variables);
  if (const YAML::Node shell = node["window_shell"]; shell && shell.IsScalar()) {
    window.shell = shell.as<std::string>();
  }
  auto after = read_pairs(node["options_after"], where + ".options_after", "options");
  if (!after.has_value()) {
    return libtmux::unexpected(after.error());
  }
  window.options_after = *std::move(after);

  if (const YAML::Node options = node["options"]) {
    if (!options.IsMap()) {
      return fail(where + ".options", "options are a mapping");
    }
    for (const auto& entry : options) {
      if (!entry.first.IsScalar() || !entry.second.IsScalar()) {
        return fail(where + ".options", "an option is a name and a value");
      }
      window.options.emplace_back(entry.first.as<std::string>(),
                                  entry.second.as<std::string>());
    }
  }

  const YAML::Node panes = node["panes"];
  if (!panes) {
    // A window with no panes listed still has the one tmux gives it.
    return window;
  }
  if (!panes.IsSequence()) {
    return fail(where + ".panes", "panes are a list");
  }
  window.panes.clear();
  for (std::size_t index = 0; index < panes.size(); ++index) {
    auto pane =
        read_pane(panes[index], where + ".panes[" + std::to_string(index) + "]");
    if (!pane.has_value()) {
      return libtmux::unexpected(pane.error());
    }
    window.panes.push_back(*std::move(pane));
  }
  if (window.panes.empty()) {
    window.panes.emplace_back();
  }
  auto before =
      read_commands(node["shell_command_before"], where + ".shell_command_before");
  if (!before.has_value()) {
    return libtmux::unexpected(before.error());
  }
  prepend_to_each(window.panes, *before);
  // Applied once the panes are read: a window's default reaches every
  // command below it, and before this ran too early to reach any.
  if (const YAML::Node suppress = node["suppress_history"];
      suppress && suppress.IsScalar() && !is_true(suppress)) {
    for (Pane& pane : window.panes) {
      for (Command& command : pane.shell_commands) {
        command.suppress_history = false;
      }
    }
  }
  return window;
}

} // namespace

libtmux::expected<Workspace, ParseError> parse_tmuxp(std::string_view document) {
  YAML::Node root;
  try {
    root = YAML::Load(std::string{document});
  } catch (const YAML::Exception& error) {
    // The one place an exception can arrive from: turn it into the value the
    // rest of this consumer, and the library it uses, report failures with.
    return fail("", error.what());
  }

  if (!root.IsMap()) {
    return fail("", "a tmuxp document is a mapping");
  }
  static constexpr std::string_view kDocumentKeys[]{
      "session_name", "start_directory",      "root",
      "windows",      "shell_command_before", "environment",
      "options",      "global_options",       "suppress_history"};
  if (const auto unknown = unknown_key(root, kDocumentKeys)) {
    return fail("", "unsupported key: " + *unknown);
  }
  const YAML::Node name = root["session_name"];
  if (!name || !name.IsScalar() || name.as<std::string>().empty()) {
    return fail("session_name", "a session name is required");
  }

  Workspace workspace;
  workspace.session_name = name.as<std::string>();
  workspace.start_directory = directory_of(root);
  auto variables = read_environment(root["environment"], "environment");
  if (!variables.has_value()) {
    return libtmux::unexpected(variables.error());
  }
  workspace.environment = *std::move(variables);
  auto global = read_pairs(root["global_options"], "global_options", "options");
  if (!global.has_value()) {
    return libtmux::unexpected(global.error());
  }
  workspace.global_options = *std::move(global);
  auto session_options = read_pairs(root["options"], "options", "options");
  if (!session_options.has_value()) {
    return libtmux::unexpected(session_options.error());
  }
  workspace.options = *std::move(session_options);

  const YAML::Node windows = root["windows"];
  if (!windows || !windows.IsSequence() || windows.size() == 0) {
    return fail("windows", "at least one window is required");
  }
  workspace.windows.clear();
  for (std::size_t index = 0; index < windows.size(); ++index) {
    auto window = read_window(windows[index], "windows[" + std::to_string(index) + "]");
    if (!window.has_value()) {
      return libtmux::unexpected(window.error());
    }
    workspace.windows.push_back(*std::move(window));
  }

  // Applied after the windows are read so a window's own commands run after
  // the document's, which is the order tmuxp gives them.
  auto before = read_commands(root["shell_command_before"], "shell_command_before");
  if (!before.has_value()) {
    return libtmux::unexpected(before.error());
  }
  for (Window& window : workspace.windows) {
    prepend_to_each(window.panes, *before);
  }
  return workspace;
}

} // namespace libtmux::workspace
