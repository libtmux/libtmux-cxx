#include "libtmux_consumers/tmuxp.hpp"
#include "libtmux/expected.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
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

libtmux::expected<bool, ParseError>
read_boolean(const YAML::Node& node, const std::string& where, bool fallback = false) {
  if (!node) {
    return fallback;
  }
  bool value{};
  if (!node.IsScalar() || !YAML::convert<bool>::decode(node, value)) {
    return fail(where, "a boolean is required");
  }
  return value;
}

// A command is written as a string or as a mapping carrying `cmd` and how
// to send it. Both forms appear in the same list in tmuxp's own examples.
libtmux::expected<Command, ParseError> read_command(const YAML::Node& node,
                                                    const std::string& where,
                                                    Command command = {},
                                                    bool require_text = true) {
  if (!node || node.IsNull()) {
    // A blank entry opens a pane and runs nothing in it.
    return command;
  }
  if (node.IsScalar()) {
    command.text = node.as<std::string>();
    return command;
  }
  if (!node.IsMap()) {
    return fail(where, "a command is a string or a mapping");
  }
  static constexpr std::string_view kCommandKeys[]{"cmd", "enter", "sleep_before",
                                                   "sleep_after", "suppress_history"};
  if (const auto unknown = unknown_key(node, kCommandKeys)) {
    return fail(where, "unsupported key: " + *unknown);
  }
  const YAML::Node text = node["cmd"];
  if (require_text && (!text || !text.IsScalar())) {
    return fail(where + ".cmd", "a command mapping needs scalar cmd text");
  }
  if (text && text.IsScalar()) {
    command.text = text.as<std::string>();
  }
  for (const auto& [key, slot] :
       {std::pair{"enter", &Command::enter},
        std::pair{"suppress_history", &Command::suppress_history}}) {
    const auto value = read_boolean(node[key], where + "." + key, command.*slot);
    if (!value.has_value()) {
      return libtmux::unexpected(value.error());
    }
    command.*slot = *value;
  }
  for (const auto& [key, slot] : {std::pair{"sleep_before", &Command::pause_before},
                                  std::pair{"sleep_after", &Command::pause_after}}) {
    const YAML::Node pause = node[key];
    if (!pause) {
      continue;
    }
    if (!pause.IsScalar()) {
      return fail(where + "." + key, "a pause is a number of seconds");
    }
    try {
      // Seconds in the document, and fractions of one are written there too.
      const auto seconds = pause.as<double>();
      if (seconds < 0.0) {
        return fail(where + "." + key, "a pause cannot be negative");
      }
      if (!std::isfinite(seconds) ||
          seconds >=
              static_cast<double>(std::numeric_limits<long long>::max()) / 1000.0) {
        return fail(where + "." + key,
                    "a pause must be finite, non-negative and representable");
      }
      command.*slot = std::chrono::milliseconds{static_cast<long long>(seconds * 1000)};
    } catch (const YAML::Exception&) {
      return fail(where + "." + key, "a pause is a number of seconds");
    }
  }
  return command;
}

void append_commands(YAML::Node& commands, const YAML::Node& source) {
  if (!source || source.IsNull()) {
    return;
  }
  const YAML::Node node =
      source.IsMap() && source["shell_command"] ? source["shell_command"] : source;
  const auto blank = [](const YAML::Node& item) {
    return item.IsNull() ||
           (item.IsScalar() && (item.Scalar() == "blank" || item.Scalar() == "pane"));
  };
  if (blank(node) || (node.IsSequence() && node.size() == 1 && blank(node[0]))) {
    return;
  }
  if (node.IsSequence()) {
    for (const auto& item : node) {
      commands.push_back(item);
    }
  } else {
    commands.push_back(node);
  }
}

libtmux::expected<std::vector<Command>, ParseError>
read_commands(const YAML::Node& node, const std::string& where, Command state) {
  std::vector<Command> commands;
  for (std::size_t index = 0; index < node.size(); ++index) {
    state.text.clear();
    auto one =
        read_command(node[index], where + "[" + std::to_string(index) + "]", state);
    if (!one.has_value()) {
      return libtmux::unexpected(one.error());
    }
    state = *one;
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
                                              const std::string& where,
                                              const YAML::Node& inherited_commands,
                                              bool suppress_history) {
  Pane pane;
  YAML::Node commands{YAML::NodeType::Sequence};
  append_commands(commands, inherited_commands);
  Command state{.suppress_history = suppress_history};
  if (node.IsMap()) {
    static constexpr std::string_view kPaneKeys[]{
        "shell_command", "start_directory",  "root",  "shell_command_before",
        "focus",         "environment",      "enter", "sleep_before",
        "sleep_after",   "suppress_history", "shell"};
    if (const auto unknown = unknown_key(node, kPaneKeys)) {
      return fail(where, "unsupported key: " + *unknown);
    }
    pane.start_directory = directory_of(node);
    const auto focus = read_boolean(node["focus"], where + ".focus");
    if (!focus.has_value()) {
      return libtmux::unexpected(focus.error());
    }
    pane.focus = *focus;
    auto variables = read_environment(node["environment"], where + ".environment");
    if (!variables.has_value()) {
      return libtmux::unexpected(variables.error());
    }
    pane.environment = *std::move(variables);
    pane.environment_overrides = static_cast<bool>(node["environment"]);
    if (const YAML::Node shell = node["shell"]; shell && shell.IsScalar()) {
      pane.shell = shell.as<std::string>();
    }
    YAML::Node defaults{YAML::NodeType::Map};
    for (const auto* key :
         {"enter", "sleep_before", "sleep_after", "suppress_history"}) {
      if (node[key]) {
        defaults[key] = node[key];
      }
    }
    auto initial = read_command(defaults, where, state, false);
    if (!initial.has_value()) {
      return libtmux::unexpected(initial.error());
    }
    state = *initial;
    append_commands(commands, node["shell_command_before"]);
    append_commands(commands, node["shell_command"]);
  } else {
    append_commands(commands, node);
  }
  auto parsed = read_commands(commands, where + ".shell_command", state);
  if (!parsed.has_value()) {
    return libtmux::unexpected(parsed.error());
  }
  pane.shell_commands = *std::move(parsed);
  return pane;
}

libtmux::expected<Window, ParseError> read_window(const YAML::Node& node,
                                                  const std::string& where,
                                                  const YAML::Node& inherited_commands,
                                                  bool suppress_history) {
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
  const auto focus = read_boolean(node["focus"], where + ".focus");
  if (!focus.has_value()) {
    return libtmux::unexpected(focus.error());
  }
  window.focus = *focus;
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

  YAML::Node commands{YAML::NodeType::Sequence};
  append_commands(commands, inherited_commands);
  append_commands(commands, node["shell_command_before"]);
  const auto suppress = read_boolean(node["suppress_history"],
                                     where + ".suppress_history", suppress_history);
  if (!suppress.has_value()) {
    return libtmux::unexpected(suppress.error());
  }
  suppress_history = *suppress;
  const YAML::Node panes = node["panes"];
  if (panes && !panes.IsSequence()) {
    return fail(where + ".panes", "panes are a list");
  }
  window.panes.clear();
  const auto count = panes && panes.size() != 0 ? panes.size() : 1U;
  for (std::size_t index = 0; index < count; ++index) {
    const YAML::Node source = panes && panes.size() != 0 ? panes[index] : YAML::Node{};
    auto pane = read_pane(source, where + ".panes[" + std::to_string(index) + "]",
                          commands, suppress_history);
    if (!pane.has_value()) {
      return libtmux::unexpected(pane.error());
    }
    window.panes.push_back(*std::move(pane));
  }
  return window;
}

} // namespace

libtmux::expected<Workspace, ParseError> parse_tmuxp(std::string_view document) {
  YAML::Node root;
  try {
    const auto documents = YAML::LoadAll(std::string{document});
    if (documents.size() != 1) {
      return fail("", "exactly one YAML document is required");
    }
    root = documents.front();
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
  const auto suppress =
      read_boolean(root["suppress_history"], "suppress_history", true);
  if (!suppress.has_value()) {
    return libtmux::unexpected(suppress.error());
  }
  workspace.windows.clear();
  for (std::size_t index = 0; index < windows.size(); ++index) {
    auto window = read_window(windows[index], "windows[" + std::to_string(index) + "]",
                              root["shell_command_before"], *suppress);
    if (!window.has_value()) {
      return libtmux::unexpected(window.error());
    }
    workspace.windows.push_back(*std::move(window));
  }

  return workspace;
}

} // namespace libtmux::workspace
