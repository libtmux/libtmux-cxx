#include "libtmux_consumers/mcp.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "libtmux/capture.hpp"
#include "libtmux/format.hpp"
#include "libtmux/keys.hpp"
#include "libtmux/server.hpp"
#include "tool_support.hpp"
#include "wait_for_text.hpp"

namespace libtmux::mcp::detail {

const std::string* argument(const Arguments& arguments, std::string_view name) {
  const auto found = arguments.find(name);
  return found == arguments.end() ? nullptr : &found->second;
}

ToolOutput output(StructuredValue::Object structured,
                  std::optional<std::size_t> maximum_response_bytes) {
  return ToolOutput{.structured = std::move(structured),
                    .maximum_response_bytes = maximum_response_bytes};
}

ToolError tmux_error(const CommandFailure& error) {
  return ToolError{error.kind == FailureKind::validation, error.diagnostic};
}

StructuredValue session_value(const Session& session) {
  return StructuredValue::Object{
      {"attached", session.attached()},
      {"client_count", session.client_count()},
      {"id", session.id()},
      {"name", session.name()},
      {"path", session.path()},
      {"window_count", session.window_count()},
  };
}

StructuredValue window_value(const Window& window) {
  return StructuredValue::Object{
      {"active", window.active()},
      {"height", window.height()},
      {"id", window.id()},
      {"index", window.index()},
      {"layout", window.layout()},
      {"name", window.name()},
      {"pane_count", window.pane_count()},
      {"session_id", window.session_id()},
      {"width", window.width()},
  };
}

StructuredValue pane_value(const Pane& pane) {
  return StructuredValue::Object{
      {"active", pane.active()},
      {"command", pane.command()},
      {"dead", pane.dead()},
      {"height", pane.height()},
      {"id", pane.id()},
      {"index", pane.index()},
      {"path", pane.path()},
      {"pid", pane.pid()},
      {"session_id", pane.session_id()},
      {"title", pane.title()},
      {"width", pane.width()},
      {"window_id", pane.window_id()},
  };
}

namespace {
[[nodiscard]] ToolError cancelled() { return ToolError{false, "request cancelled"}; }
} // namespace

libtmux::expected<BoundedRegex, std::string>
BoundedRegex::compile(std::string_view pattern) {
  std::vector<Atom> atoms;
  bool escaped = false;
  for (const char value : pattern) {
    if (escaped) {
      atoms.push_back(Atom{.value = value});
      escaped = false;
      continue;
    }
    if (value == '\\') {
      escaped = true;
      continue;
    }
    if (value == '*') {
      if (atoms.empty() || atoms.back().repeated) {
        return libtmux::unexpected(std::string{"misplaced regex repetition"});
      }
      atoms.back().repeated = true;
      continue;
    }
    if (value == '.') {
      atoms.push_back(Atom{.any = true});
      continue;
    }
    if (std::string_view{"+?[](){}|^$"}.find(value) != std::string_view::npos) {
      return libtmux::unexpected(
          std::string{"unsupported regex operator; escape it for a literal"});
    }
    atoms.push_back(Atom{.value = value});
  }
  if (escaped || atoms.empty()) {
    return libtmux::unexpected(std::string{"incomplete or empty regex"});
  }
  return BoundedRegex{std::move(atoms)};
}

libtmux::expected<bool, std::string>
BoundedRegex::search(std::string_view text, std::size_t& remaining_work) const {
  const auto charge = [&remaining_work](std::size_t work) {
    if (work > remaining_work) {
      return false;
    }
    remaining_work -= work;
    return true;
  };
  if (!charge(atoms_.size())) {
    return libtmux::unexpected(std::string{"search matching work limit exceeded"});
  }
  std::vector<bool> previous(atoms_.size() + 1U);
  std::vector<bool> next(atoms_.size() + 1U);
  previous[0] = true;
  for (std::size_t index = 1U; index <= atoms_.size(); ++index) {
    previous[index] = atoms_[index - 1U].repeated && previous[index - 1U];
  }
  if (previous.back()) {
    return true;
  }
  for (const char character : text) {
    if (!charge(atoms_.size())) {
      return libtmux::unexpected(std::string{"search matching work limit exceeded"});
    }
    next.assign(atoms_.size() + 1U, false);
    next[0] = true;
    for (std::size_t index = 1U; index <= atoms_.size(); ++index) {
      const Atom& atom = atoms_[index - 1U];
      const bool matches = atom.any || atom.value == character;
      next[index] = atom.repeated ? next[index - 1U] || (matches && previous[index])
                                  : matches && previous[index - 1U];
    }
    if (next.back()) {
      return true;
    }
    previous.swap(next);
  }
  return false;
}
} // namespace libtmux::mcp::detail

namespace libtmux::mcp {
namespace {

using namespace std::chrono_literals;

struct Field {
  Parameter parameter;
  std::set<Sink> sinks;
};

void append_json_string(std::string_view value, std::string& output) {
  constexpr std::string_view hex = "0123456789abcdef";
  output.push_back('"');
  for (const char byte : value) {
    const auto character = static_cast<unsigned char>(byte);
    switch (character) {
    case '"':
      output += "\\\"";
      break;
    case '\\':
      output += "\\\\";
      break;
    case '\b':
      output += "\\b";
      break;
    case '\f':
      output += "\\f";
      break;
    case '\n':
      output += "\\n";
      break;
    case '\r':
      output += "\\r";
      break;
    case '\t':
      output += "\\t";
      break;
    default:
      if (character < 0x20U) {
        output += "\\u00";
        output.push_back(hex[character >> 4U]);
        output.push_back(hex[character & 0x0FU]);
      } else {
        output.push_back(static_cast<char>(character));
      }
    }
  }
  output.push_back('"');
}

void append_json(const StructuredValue& value, std::string& output) {
  std::visit(
      [&](const auto& item) {
        using Item = std::decay_t<decltype(item)>;
        if constexpr (std::same_as<Item, std::nullptr_t>) {
          output += "null";
        } else if constexpr (std::same_as<Item, bool>) {
          output += item ? "true" : "false";
        } else if constexpr (std::same_as<Item, std::int64_t>) {
          std::array<char, 32> digits{};
          const auto converted =
              std::to_chars(digits.data(), digits.data() + digits.size(), item);
          output.append(digits.data(), converted.ptr);
        } else if constexpr (std::same_as<Item, std::string>) {
          append_json_string(item, output);
        } else if constexpr (std::same_as<Item, StructuredValue::Array>) {
          output.push_back('[');
          bool first = true;
          for (const StructuredValue& nested : item) {
            if (!first) {
              output.push_back(',');
            }
            first = false;
            append_json(nested, output);
          }
          output.push_back(']');
        } else {
          output.push_back('{');
          bool first = true;
          for (const auto& [key, nested] : item) {
            if (!first) {
              output.push_back(',');
            }
            first = false;
            append_json_string(key, output);
            output.push_back(':');
            append_json(nested, output);
          }
          output.push_back('}');
        }
      },
      value.value);
}

[[nodiscard]] std::string serialize_json(const StructuredValue& value) {
  std::string output;
  append_json(value, output);
  return output;
}

[[nodiscard]] StructuredValue nested_result(const ToolOutput& answer) {
  StructuredValue structured{answer.structured};
  const std::string text = serialize_json(structured);
  return StructuredValue::Object{
      {"content", StructuredValue::Array{StructuredValue::Object{{"text", text},
                                                                 {"type", "text"}}}},
      {"isError", false},
      {"structuredContent", std::move(structured)},
  };
}

[[nodiscard]] StructuredValue nested_error(std::string_view message) {
  return StructuredValue::Object{
      {"content", StructuredValue::Array{StructuredValue::Object{{"text", message},
                                                                 {"type", "text"}}}},
      {"isError", true},
  };
}

[[nodiscard]] Field field(std::string name, std::string description, InputSink type,
                          bool required = true,
                          ArgumentType argument_type = ArgumentType::string,
                          std::optional<long long> minimum = {},
                          std::optional<long long> maximum = {},
                          std::optional<std::size_t> maximum_length = {},
                          NestedAuthority nested = NestedAuthority::none,
                          InputControl control = InputControl::none,
                          std::vector<std::string> allowed_values = {}) {
  std::set<Sink> sinks{Sink{type, nested, control}};
  if (type == InputSink::tmux_format && control == InputControl::double_hash_once) {
    sinks.insert(
        Sink{InputSink::tmux_state, NestedAuthority::none, InputControl::none});
  } else if (type == InputSink::tmux_format &&
             control == InputControl::validated_variable_name) {
    sinks.insert(
        Sink{InputSink::tmux_lookup, NestedAuthority::none, InputControl::none});
  }
  return Field{.parameter = {.name = std::move(name),
                             .description = std::move(description),
                             .type = argument_type,
                             .required = required,
                             .minimum = minimum,
                             .maximum = maximum,
                             .maximum_length = maximum_length,
                             .allowed_values = std::move(allowed_values)},
               .sinks = std::move(sinks)};
}

[[nodiscard]] Field send_key_operations_field() {
  return Field{
      .parameter = {.name = "operations",
                    .description = "One to sixty-four ordered pane-input operations.",
                    .type = ArgumentType::send_key_operations,
                    .required = true,
                    .minimum = std::nullopt,
                    .maximum = std::nullopt,
                    .maximum_length = std::nullopt,
                    .allowed_values = {}},
      .sinks = {Sink{InputSink::tmux_lookup, NestedAuthority::none},
                Sink{InputSink::pane_input, NestedAuthority::unrestricted}},
  };
}

[[nodiscard]] std::string_view opener(Toolset toolset, ProcessReach reach,
                                      const std::set<OutputClass>& outputs) {
  switch (reach) {
  case ProcessReach::configured_process:
    return "Start a pane's configured process; accepts no command payload.";
  case ProcessReach::pane_input:
    return "Send input to a pane's program; a shell that receives it runs it with "
           "your user's permissions.";
  case ProcessReach::pane_command:
    return "Run a shell command in a pane with your user's permissions.";
  case ProcessReach::none:
    break;
  }
  if (toolset == Toolset::teardown) {
    return "Delete tmux state; accepts no command payload.";
  }
  if (outputs.contains(OutputClass::terminal_content)) {
    return "Read pane output; accepts no client-supplied executable input. Returned "
           "content may be sensitive or untrusted.";
  }
  if (outputs.contains(OutputClass::process_environment)) {
    return "Read the tmux environment; accepts no client-supplied executable input. "
           "Returned values may contain secrets.";
  }
  if (outputs.contains(OutputClass::configured_command)) {
    return "Read configured tmux commands; accepts no client-supplied executable "
           "input. Returned values may contain executable configuration.";
  }
  if (toolset == Toolset::manage || toolset == Toolset::execute) {
    return "Change tmux state; no client-supplied executable input.";
  }
  return "Inspect tmux metadata; accepts no client-supplied executable input.";
}

[[nodiscard]] ToolDefinition
make_tool(std::string name, std::string title, Toolset toolset, ProcessReach reach,
          std::set<Effect> effects, std::set<OutputClass> outputs, bool secrets,
          bool untrusted, ToolAnnotations annotations, std::vector<Field> fields,
          OutputShape output_shape, Handler handler, std::string description,
          bool amplifies_future_input = false,
          std::set<std::string, std::less<>> nested_tools = {}) {
  ToolDefinition tool{.name = std::move(name),
                      .title = std::move(title),
                      .description = {},
                      .toolset = toolset,
                      .authority = {.process_reach = reach,
                                    .effects = std::move(effects),
                                    .output_classes = std::move(outputs),
                                    .may_expose_secrets = secrets,
                                    .may_return_untrusted_content = untrusted,
                                    .amplifies_future_input = amplifies_future_input,
                                    .input_sinks = {},
                                    .nested_tools = std::move(nested_tools)},
                      .annotations = annotations,
                      .schema = {.input = {}, .output = output_shape},
                      .handler = std::move(handler)};
  tool.description = std::string{opener(tool.toolset, tool.authority.process_reach,
                                        tool.authority.output_classes)};
  if (!description.empty()) {
    tool.description.push_back(' ');
    tool.description += std::move(description);
  }
  for (Field& item : fields) {
    tool.authority.input_sinks.emplace(item.parameter.name, std::move(item.sinks));
    tool.schema.input.push_back(std::move(item.parameter));
  }
  return tool;
}

[[nodiscard]] const std::string& required(const FlatArguments& arguments,
                                          std::string_view name) {
  return arguments.find(name)->second;
}

[[nodiscard]] std::string optional(const FlatArguments& arguments,
                                   std::string_view name) {
  const auto found = arguments.find(name);
  return found == arguments.end() ? std::string{} : found->second;
}

[[nodiscard]] long long integer(const FlatArguments& arguments, std::string_view name,
                                long long fallback = 0) {
  const auto found = arguments.find(name);
  if (found == arguments.end()) {
    return fallback;
  }
  long long value = fallback;
  static_cast<void>(std::from_chars(
      found->second.data(), found->second.data() + found->second.size(), value));
  return value;
}

[[nodiscard]] bool boolean(const FlatArguments& arguments, std::string_view name,
                           bool fallback = false) {
  const auto found = arguments.find(name);
  return found == arguments.end() ? fallback : found->second == "true";
}

[[nodiscard]] ToolResult failure(const CommandFailure& error) {
  return libtmux::unexpected(detail::tmux_error(error));
}

[[nodiscard]] ToolResult changed(std::string_view key, std::string_view id) {
  return detail::output({{std::string{key}, id}, {"changed", StructuredValue{true}}});
}

[[nodiscard]] libtmux::expected<StructuredValue::Array, CommandFailure>
resolved_pane_targets(const Pane& pane) {
  const auto window = pane.window();
  if (!window.has_value()) {
    return libtmux::unexpected(window.error());
  }
  const auto synchronized = window->option("synchronize-panes");
  if (!synchronized.has_value()) {
    return libtmux::unexpected(synchronized.error());
  }
  if (synchronized->value != "on") {
    return StructuredValue::Array{pane.id()};
  }
  const auto panes = window->panes();
  if (!panes.has_value()) {
    return libtmux::unexpected(panes.error());
  }
  std::vector<std::string> ids;
  ids.reserve(panes->size());
  for (const Pane& candidate : *panes) {
    ids.emplace_back(candidate.id());
  }
  std::ranges::sort(ids);
  StructuredValue::Array targets;
  targets.reserve(ids.size());
  for (std::string& id : ids) {
    targets.emplace_back(std::move(id));
  }
  return targets;
}

[[nodiscard]] StructuredValue option_value(const OptionEntry& option) {
  StructuredValue::Object result{
      {"inherited", option.inherited}, {"name", option.name}, {"value", option.value}};
  if (option.index.has_value()) {
    result.emplace("index", static_cast<long long>(*option.index));
  }
  return result;
}

[[nodiscard]] std::vector<std::string_view> lines(std::string_view value) {
  std::vector<std::string_view> result;
  std::size_t offset = 0;
  while (offset <= value.size()) {
    const std::size_t end = value.find('\n', offset);
    const std::string_view line = value.substr(
        offset, end == std::string_view::npos ? std::string_view::npos : end - offset);
    if (!line.empty()) {
      result.push_back(line);
    }
    if (end == std::string_view::npos) {
      break;
    }
    offset = end + 1U;
  }
  return result;
}

[[nodiscard]] bool valid_variable(std::string_view name) {
  return !name.empty() && std::ranges::all_of(name, [](char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_';
  });
}

[[nodiscard]] ToolResult list_sessions_impl(const Server& server) {
  const auto sessions = server.sessions();
  if (!sessions.has_value()) {
    if (sessions.error().kind == FailureKind::missing) {
      return detail::output({{"sessions", StructuredValue::Array{}}});
    }
    return failure(sessions.error());
  }
  StructuredValue::Array rows;
  for (const Session& session : *sessions) {
    rows.push_back(detail::session_value(session));
  }
  return detail::output({{"sessions", StructuredValue{std::move(rows)}}});
}

[[nodiscard]] ToolResult list_panes_impl(const Server& server) {
  const auto panes = server.panes();
  if (!panes.has_value()) {
    if (panes.error().kind == FailureKind::missing) {
      return detail::output({{"panes", StructuredValue::Array{}}});
    }
    return failure(panes.error());
  }
  StructuredValue::Array rows;
  for (const Pane& pane : *panes) {
    rows.push_back(detail::pane_value(pane));
  }
  return detail::output({{"panes", StructuredValue{std::move(rows)}}});
}

[[nodiscard]] std::vector<ToolDefinition> definitions() {
  std::vector<ToolDefinition> tools;
  tools.reserve(45U);
  const std::set<std::string, std::less<>> read_batch_tools{
      "list_sessions",      "list_windows",     "list_panes",
      "get_server_info",    "get_session_info", "get_window_info",
      "get_pane_info",      "capture_pane",     "capture_since",
      "snapshot_pane",      "search_panes",     "find_pane_by_position",
      "get_tmux_variables", "show_option",      "show_environment",
      "show_hooks"};
  const auto add = [&tools](ToolDefinition tool) { tools.push_back(std::move(tool)); };

  add(make_tool(
      "list_sessions", "List tmux sessions", Toolset::inspect, ProcessReach::none,
      {Effect::observe}, {OutputClass::tmux_metadata}, false, false,
      kConservativeAnnotations, {}, OutputShape::sessions,
      [](const Server& server, const Arguments&, const CallContext&) {
        return list_sessions_impl(server);
      },
      "List stable session IDs, names, paths, and client counts."));

  add(make_tool(
      "list_windows", "List tmux windows", Toolset::inspect, ProcessReach::none,
      {Effect::observe}, {OutputClass::tmux_metadata}, false, false,
      kConservativeAnnotations,
      {field("session", "Stable session ID or name.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters)},
      OutputShape::windows,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto session = server.session(required(arguments, "session"));
        if (!session.has_value()) {
          return failure(session.error());
        }
        const auto windows = session->windows();
        if (!windows.has_value()) {
          return failure(windows.error());
        }
        StructuredValue::Array rows;
        for (const Window& window : *windows) {
          rows.push_back(detail::window_value(window));
        }
        return detail::output({{"windows", StructuredValue{std::move(rows)}}});
      },
      "List windows through an exact owning session."));

  add(make_tool(
      "list_panes", "List tmux panes", Toolset::inspect, ProcessReach::none,
      {Effect::observe}, {OutputClass::tmux_metadata}, false, false,
      kConservativeAnnotations, {}, OutputShape::panes,
      [](const Server& server, const Arguments&, const CallContext&) {
        return list_panes_impl(server);
      },
      "List every pane with stable pane, window, and session IDs."));

  const auto inspect_metadata = [&](std::string name, std::string title,
                                    std::vector<Field> fields, OutputShape shape,
                                    Handler handler, std::string description) {
    add(make_tool(std::move(name), std::move(title), Toolset::inspect,
                  ProcessReach::none, {Effect::observe}, {OutputClass::tmux_metadata},
                  false, false, kConservativeAnnotations, std::move(fields), shape,
                  std::move(handler), std::move(description)));
  };
  const auto inspect_terminal =
      [&](std::string name, std::string title, std::vector<Field> fields,
          OutputShape shape, Handler handler, std::string description,
          std::set<OutputClass> outputs = {OutputClass::tmux_metadata,
                                           OutputClass::terminal_content}) {
        add(make_tool(std::move(name), std::move(title), Toolset::inspect,
                      ProcessReach::none, {Effect::observe}, std::move(outputs), true,
                      true, kConservativeAnnotations, std::move(fields), shape,
                      std::move(handler), std::move(description)));
      };

  inspect_metadata(
      "get_server_info", "Get tmux server information", {}, OutputShape::object,
      [](const Server& server, const Arguments&, const CallContext&) -> ToolResult {
        const bool running = server.is_alive();
        StructuredValue::Object result{{"running", running}};
        const auto version = server.tmux_version();
        if (version.has_value()) {
          std::string text = version->unbounded ? std::string{"master"}
                                                : std::to_string(version->major) + "." +
                                                      std::to_string(version->minor);
          if (version->revision != 0U) {
            text += "." + std::to_string(version->revision);
          }
          result.emplace("version", std::move(text));
        }
        if (running) {
          const auto socket = server.expand("#{socket_path}");
          if (socket.has_value()) {
            result.emplace("socket_path", *socket);
          }
        }
        return detail::output(std::move(result));
      },
      "Report liveness, tmux version, and the resolved socket when running.");

  inspect_metadata(
      "get_session_info", "Get tmux session information",
      {field("session", "Stable session ID or name.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters)},
      OutputShape::object,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto session = server.session(required(arguments, "session"));
        return session.has_value()
                   ? detail::output({{"session", detail::session_value(*session)}})
                   : failure(session.error());
      },
      "Return one session by stable ID or name.");

  inspect_metadata(
      "get_window_info", "Get tmux window information",
      {field("windowId", "Stable window ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters)},
      OutputShape::object,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto window = server.window(required(arguments, "windowId"));
        return window.has_value()
                   ? detail::output({{"window", detail::window_value(*window)}})
                   : failure(window.error());
      },
      "Return one window by stable ID.");

  inspect_metadata(
      "get_pane_info", "Get tmux pane information",
      {field("paneId", "Stable pane ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters)},
      OutputShape::object,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto pane = server.pane(required(arguments, "paneId"));
        return pane.has_value() ? detail::output({{"pane", detail::pane_value(*pane)}})
                                : failure(pane.error());
      },
      "Return one pane by stable ID.");

  inspect_terminal(
      "capture_pane", "Capture a tmux pane",
      {field("paneId", "Stable pane ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("history", "Capture retained history as well as the visible pane.",
             InputSink::none, false, ArgumentType::boolean)},
      OutputShape::pane_text,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto pane = server.pane(required(arguments, "paneId"));
        if (!pane.has_value()) {
          return failure(pane.error());
        }
        CaptureOptions options;
        options.whole_history = boolean(arguments, "history");
        const auto captured = pane->capture(options);
        return captured.has_value()
                   ? detail::output({{"pane_id", pane->id()}, {"text", *captured}})
                   : failure(captured.error());
      },
      "Return visible text, or retained history when requested.");

  inspect_terminal(
      "capture_since", "Capture new pane output",
      {field("paneId", "Stable pane ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("cursor", "Previously returned byte cursor.", InputSink::none, false,
             ArgumentType::integer, 0)},
      OutputShape::object,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto pane = server.pane(required(arguments, "paneId"));
        if (!pane.has_value()) {
          return failure(pane.error());
        }
        CaptureOptions options;
        options.whole_history = true;
        const auto captured = pane->capture(options);
        if (!captured.has_value()) {
          return failure(captured.error());
        }
        std::size_t cursor = static_cast<std::size_t>(integer(arguments, "cursor"));
        if (cursor > captured->size()) {
          cursor = 0U;
        }
        return detail::output({{"cursor", static_cast<long long>(captured->size())},
                               {"pane_id", pane->id()},
                               {"text", captured->substr(cursor)}});
      },
      "Return bytes after a bounded client cursor and a replacement cursor.");

  inspect_terminal("snapshot_pane", "Snapshot a tmux pane",
                   {field("paneId", "Stable pane ID.", InputSink::tmux_lookup, true,
                          ArgumentType::string, {}, {}, detail::kTargetCharacters)},
                   OutputShape::object,
                   [](const Server& server, const Arguments& arguments,
                      const CallContext&) -> ToolResult {
                     const auto pane = server.pane(required(arguments, "paneId"));
                     if (!pane.has_value()) {
                       return failure(pane.error());
                     }
                     const auto captured = pane->capture();
                     return captured.has_value()
                                ? detail::output({{"pane", detail::pane_value(*pane)},
                                                  {"text", *captured}})
                                : failure(captured.error());
                   },
                   "Return pane metadata and visible terminal content from one call.",
                   {OutputClass::tmux_metadata, OutputClass::terminal_content});

  inspect_terminal(
      "search_panes", "Search tmux panes",
      {field("pattern", "Bounded regular expression.", InputSink::regex, true,
             ArgumentType::string, {}, {}, 256U)},
      OutputShape::matches,
      [](const Server& server, const Arguments& arguments,
         const CallContext& context) -> ToolResult {
        const auto pattern =
            detail::BoundedRegex::compile(required(arguments, "pattern"));
        if (!pattern.has_value()) {
          return libtmux::unexpected(ToolError{true, pattern.error()});
        }
        const auto panes = server.panes();
        if (!panes.has_value()) {
          return failure(panes.error());
        }
        StructuredValue::Array matches;
        std::size_t remaining_work = detail::kRegexWorkUnits;
        for (const Pane& pane : *panes) {
          if (context.cancelled()) {
            return libtmux::unexpected(detail::cancelled());
          }
          const auto captured = pane.capture();
          if (!captured.has_value()) {
            return failure(captured.error());
          }
          for (const std::string_view line : libtmux::capture_lines(*captured)) {
            const auto matched = pattern->search(line, remaining_work);
            if (!matched.has_value()) {
              return libtmux::unexpected(ToolError{false, matched.error()});
            }
            if (*matched) {
              if (matches.size() == detail::kSearchMatchLimit) {
                return libtmux::unexpected(
                    ToolError{false, "search match limit exceeded"});
              }
              matches.push_back(
                  StructuredValue::Object{{"line", line}, {"pane_id", pane.id()}});
            }
          }
        }
        return detail::output({{"matches", StructuredValue{std::move(matches)}}});
      },
      "Search captured lines using a length-limited expression.");

  inspect_metadata(
      "find_pane_by_position", "Find a pane by position",
      {field("row", "Zero-based terminal row.", InputSink::none, true,
             ArgumentType::integer, 0),
       field("column", "Zero-based terminal column.", InputSink::none, true,
             ArgumentType::integer, 0),
       field("windowId", "Optional stable window ID.", InputSink::tmux_lookup, false,
             ArgumentType::string, {}, {}, detail::kTargetCharacters)},
      OutputShape::object,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto reply = server.run({"list-panes", "-a", "-F",
                                       "#{pane_id}\t#{window_id}\t#{pane_left}\t"
                                       "#{pane_top}\t#{pane_width}\t#{pane_height}"});
        if (!reply.has_value()) {
          return failure(reply.error());
        }
        const long long row = integer(arguments, "row");
        const long long column = integer(arguments, "column");
        const std::string window = optional(arguments, "windowId");
        for (const std::string_view line : lines(*reply)) {
          std::vector<std::string_view> parts;
          std::size_t offset = 0U;
          while (offset <= line.size()) {
            const auto end = line.find('\t', offset);
            parts.push_back(line.substr(offset, end == std::string_view::npos
                                                    ? std::string_view::npos
                                                    : end - offset));
            if (end == std::string_view::npos) {
              break;
            }
            offset = end + 1U;
          }
          if (parts.size() != 6U || (!window.empty() && parts[1] != window)) {
            continue;
          }
          const auto number = [](std::string_view text) {
            long long result = 0;
            static_cast<void>(
                std::from_chars(text.data(), text.data() + text.size(), result));
            return result;
          };
          const long long left = number(parts[2]);
          const long long top = number(parts[3]);
          const long long width = number(parts[4]);
          const long long height = number(parts[5]);
          if (column >= left && column < left + width && row >= top &&
              row < top + height) {
            return detail::output({{"pane_id", parts[0]}, {"window_id", parts[1]}});
          }
        }
        return libtmux::unexpected(ToolError{true, "no pane contains that position"});
      },
      "Resolve coordinates using fixed tmux layout fields.");

  add(make_tool("wait_for_text", "Wait for pane text", Toolset::inspect,
                ProcessReach::none, {Effect::observe},
                {OutputClass::tmux_metadata, OutputClass::terminal_content}, true, true,
                kConservativeAnnotations,
                {field("target", "Pane ID or pane target.", InputSink::tmux_lookup,
                       true, ArgumentType::string, {}, {}, detail::kTargetCharacters),
                 field("text", "Literal text to wait for.", InputSink::none, true,
                       ArgumentType::string, {}, {}, detail::kSearchCharacters),
                 field("timeout_ms", "Bounded wait in milliseconds.", InputSink::none,
                       false, ArgumentType::integer, 1, 60000)},
                OutputShape::wait, detail::wait_for_text,
                "Poll within one deadline and report a match or timeout."));

  add(make_tool(
      "get_tmux_variables", "Get tmux variables", Toolset::inspect, ProcessReach::none,
      {Effect::observe}, {OutputClass::tmux_metadata, OutputClass::configured_command},
      false, true, kConservativeAnnotations,
      {field("names", "One to thirty-two validated tmux variable names.",
             InputSink::tmux_format, true, ArgumentType::string_array, 1, 32, 128U,
             NestedAuthority::controlled, InputControl::validated_variable_name),
       field("paneId", "Optional pane context.", InputSink::tmux_lookup, false,
             ArgumentType::string, {}, {}, detail::kTargetCharacters)},
      OutputShape::object,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto& names = arguments.string_arrays.at("names");
        if (names.empty() || !std::ranges::all_of(names, valid_variable)) {
          return libtmux::unexpected(
              ToolError{true, "names must contain valid tmux variable names"});
        }
        std::string format;
        for (const std::string& name : names) {
          if (!format.empty()) {
            format.push_back('\t');
          }
          format += libtmux::variable(name);
        }
        libtmux::expected<std::string, CommandFailure> expanded = server.expand(format);
        if (const std::string pane_id = optional(arguments, "paneId");
            !pane_id.empty()) {
          const auto pane = server.pane(pane_id);
          if (!pane.has_value()) {
            return failure(pane.error());
          }
          expanded = pane->expand(format);
        }
        if (!expanded.has_value()) {
          return failure(expanded.error());
        }
        StructuredValue::Object values;
        std::size_t offset = 0U;
        for (const std::string& name : names) {
          const auto end = expanded->find('\t', offset);
          values.emplace(name, expanded->substr(offset, end == std::string::npos
                                                            ? std::string::npos
                                                            : end - offset));
          offset = end == std::string::npos ? expanded->size() : end + 1U;
        }
        return detail::output({{"values", StructuredValue{std::move(values)}}});
      },
      "Expand only validated variable identifiers, never a caller format."));

  add(make_tool(
      "show_option", "Show a tmux option", Toolset::inspect, ProcessReach::none,
      {Effect::observe}, {OutputClass::tmux_metadata, OutputClass::configured_command},
      false, true, kConservativeAnnotations,
      {field("name", "Exact option name.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, 256U),
       field("target", "Optional session, window, or pane target.",
             InputSink::tmux_lookup, false, ArgumentType::string, {}, {},
             detail::kTargetCharacters)},
      OutputShape::object,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto options = server.options(optional(arguments, "target"));
        if (!options.has_value()) {
          return failure(options.error());
        }
        const auto found = std::ranges::find(*options, required(arguments, "name"),
                                             &OptionEntry::name);
        return found == options->end()
                   ? ToolResult{libtmux::unexpected(
                         ToolError{true, "tmux option is not set"})}
                   : ToolResult{detail::output({{"option", option_value(*found)}})};
      },
      "Read one exact option without accepting an option value."));

  add(make_tool(
      "show_environment", "Show the tmux environment", Toolset::inspect,
      ProcessReach::none, {Effect::observe}, {OutputClass::process_environment}, true,
      false, kConservativeAnnotations,
      {field("session", "Optional session target.", InputSink::tmux_lookup, false,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("name", "Optional exact environment name.", InputSink::tmux_lookup, false,
             ArgumentType::string, {}, {}, 256U)},
      OutputShape::object,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        CommandRequest request{"show-environment"};
        if (const std::string session = optional(arguments, "session");
            !session.empty()) {
          request.emplace_back("-t");
          request.emplace_back(session);
        } else {
          request.emplace_back("-g");
        }
        if (const std::string name = optional(arguments, "name"); !name.empty()) {
          request.emplace_back(name);
        }
        const auto reply = server.run(request);
        if (!reply.has_value()) {
          return failure(reply.error());
        }
        StructuredValue::Object values;
        for (const std::string_view line : lines(*reply)) {
          const auto equal = line.find('=');
          if (equal == std::string_view::npos) {
            values.emplace(std::string{line}, StructuredValue{});
          } else {
            values.emplace(std::string{line.substr(0, equal)}, line.substr(equal + 1U));
          }
        }
        return detail::output({{"environment", StructuredValue{std::move(values)}}});
      },
      "Read global or session environment values."));

  add(make_tool(
      "show_hooks", "Show tmux hooks", Toolset::inspect, ProcessReach::none,
      {Effect::observe}, {OutputClass::configured_command}, false, true,
      kConservativeAnnotations,
      {field("target", "Optional tmux target.", InputSink::tmux_lookup, false,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("name", "Optional exact hook name.", InputSink::tmux_lookup, false,
             ArgumentType::string, {}, {}, 256U)},
      OutputShape::object,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto hooks = server.hooks(optional(arguments, "target"));
        if (!hooks.has_value()) {
          return failure(hooks.error());
        }
        const std::string wanted = optional(arguments, "name");
        StructuredValue::Array rows;
        for (const OptionEntry& hook : *hooks) {
          if (wanted.empty() || hook.name == wanted) {
            rows.push_back(option_value(hook));
          }
        }
        return detail::output({{"hooks", StructuredValue{std::move(rows)}}});
      },
      "Read configured hooks, optionally filtered by exact name."));

  add(make_tool(
      "call_read_tools_batch", "Call read tools as a batch", Toolset::inspect,
      ProcessReach::none, {Effect::observe},
      {OutputClass::tmux_metadata, OutputClass::terminal_content,
       OutputClass::process_environment, OutputClass::configured_command},
      true, true, kConservativeAnnotations,
      {field("operations", "One to sixteen typed inspect-tool operations.",
             InputSink::nested_tool, true, ArgumentType::read_calls, {}, {}, {},
             NestedAuthority::controlled),
       field("onError", "Stop after the first failed operation or continue.",
             InputSink::none, false, ArgumentType::string, {}, {}, {},
             NestedAuthority::none, InputControl::none, {"continue", "stop"})},
      OutputShape::object,
      [](const Server&, const Arguments& arguments,
         const CallContext& context) -> ToolResult {
        if (!context.call_tool || !context.can_call_read_tool) {
          return libtmux::unexpected(
              ToolError{false, "nested tool dispatch is unavailable"});
        }
        if (arguments.read_calls.empty() || arguments.read_calls.size() > 16U) {
          return libtmux::unexpected(ToolError{
              true, "operations must contain between one and sixteen operations"});
        }
        std::string on_error = optional(arguments, "onError");
        if (on_error.empty()) {
          on_error = "stop";
        }
        if (on_error != "stop" && on_error != "continue") {
          return libtmux::unexpected(
              ToolError{true, "onError must be stop or continue"});
        }
        StructuredValue::Array results;
        std::size_t succeeded = 0U;
        std::size_t failed = 0U;
        std::optional<std::size_t> stopped_at;
        for (std::size_t index = 0U; index < arguments.read_calls.size(); ++index) {
          const ReadToolCall& call = arguments.read_calls[index];
          if (!context.can_call_read_tool(call.tool)) {
            ++failed;
            const std::string message = "tool is not batch eligible: " + call.tool;
            results.push_back(
                StructuredValue::Object{{"error", message},
                                        {"index", static_cast<long long>(index)},
                                        {"result", nested_error(message)},
                                        {"resultTruncated", false},
                                        {"success", false},
                                        {"tool", call.tool}});
            if (on_error == "stop") {
              stopped_at = index;
              break;
            }
            continue;
          }
          ToolResult result = context.call_tool(call.tool, Arguments{call.arguments});
          if (!result.has_value()) {
            ++failed;
            results.push_back(StructuredValue::Object{
                {"error", result.error().message},
                {"index", static_cast<long long>(index)},
                {"result", nested_error(result.error().message)},
                {"resultTruncated", false},
                {"success", false},
                {"tool", call.tool}});
            if (on_error == "stop") {
              stopped_at = index;
              break;
            }
            continue;
          }
          ++succeeded;
          results.push_back(
              StructuredValue::Object{{"error", StructuredValue{}},
                                      {"index", static_cast<long long>(index)},
                                      {"result", nested_result(*result)},
                                      {"resultTruncated", false},
                                      {"success", true},
                                      {"tool", call.tool}});
        }
        return detail::output(
            {{"failed", static_cast<long long>(failed)},
             {"onError", on_error},
             {"results", StructuredValue{std::move(results)}},
             {"stoppedAt", stopped_at.has_value()
                               ? StructuredValue{static_cast<long long>(*stopped_at)}
                               : StructuredValue{}},
             {"succeeded", static_cast<long long>(succeeded)},
             {"truncated", false},
             {"truncatedBytes", 0}},
            detail::kReadBatchResponseBytes);
      },
      "Run up to sixteen declared inspect operations serially under this one call; "
      "inner operations receive no separate approval.",
      false, read_batch_tools));
  const auto manage = [&](std::string name, std::string title,
                          std::vector<Field> fields, Handler handler,
                          std::string description) {
    const bool state_only = name == "wait_for_channel" || name == "signal_channel" ||
                            name == "set_mouse_enabled" || name == "set_history_limit";
    add(make_tool(std::move(name), std::move(title), Toolset::manage,
                  ProcessReach::none,
                  state_only ? std::set{Effect::change}
                             : std::set{Effect::observe, Effect::change},
                  {OutputClass::tmux_metadata}, false, false, kConservativeAnnotations,
                  std::move(fields), OutputShape::object, std::move(handler),
                  std::move(description)));
  };

  manage(
      "rename_session", "Rename a tmux session",
      {field("session", "Stable session ID or name.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("name", "Literal replacement session name.", InputSink::tmux_format, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters,
             NestedAuthority::controlled, InputControl::double_hash_once)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto session = server.session(required(arguments, "session"));
        if (!session.has_value()) {
          return failure(session.error());
        }
        const auto answer = session->rename(required(arguments, "name"));
        return answer.has_value() ? changed("session_id", session->id())
                                  : failure(answer.error());
      },
      "Replace one session name.");

  manage(
      "rename_window", "Rename a tmux window",
      {field("windowId", "Stable window ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("name", "Literal replacement window name.", InputSink::tmux_format, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters,
             NestedAuthority::controlled, InputControl::double_hash_once)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto window = server.window(required(arguments, "windowId"));
        if (!window.has_value()) {
          return failure(window.error());
        }
        const auto answer = window->rename(required(arguments, "name"));
        return answer.has_value() ? changed("window_id", window->id())
                                  : failure(answer.error());
      },
      "Replace one window name.");

  manage(
      "select_window", "Select a tmux window",
      {field("windowId", "Stable window ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto window = server.window(required(arguments, "windowId"));
        if (!window.has_value()) {
          return failure(window.error());
        }
        const auto answer = window->select();
        return answer.has_value() ? changed("window_id", window->id())
                                  : failure(answer.error());
      },
      "Make one window active.");

  manage(
      "select_pane", "Select a tmux pane",
      {field("paneId", "Stable pane ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto pane = server.pane(required(arguments, "paneId"));
        if (!pane.has_value()) {
          return failure(pane.error());
        }
        const auto answer = pane->select();
        return answer.has_value() ? changed("pane_id", pane->id())
                                  : failure(answer.error());
      },
      "Make one pane active.");

  manage(
      "select_layout", "Select a tmux layout",
      {field("windowId", "Stable window ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("layout", "Named or saved tmux layout.", InputSink::tmux_state, true,
             ArgumentType::string, {}, {}, 4096U)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto window = server.window(required(arguments, "windowId"));
        if (!window.has_value()) {
          return failure(window.error());
        }
        const auto answer = window->select_layout(required(arguments, "layout"));
        return answer.has_value() ? changed("window_id", window->id())
                                  : failure(answer.error());
      },
      "Replace the pane layout of one window.");

  manage(
      "resize_window", "Resize a tmux window",
      {field("windowId", "Stable window ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("width", "Positive window width.", InputSink::tmux_state, true,
             ArgumentType::integer, 1, 100000),
       field("height", "Positive window height.", InputSink::tmux_state, true,
             ArgumentType::integer, 1, 100000)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto window = server.window(required(arguments, "windowId"));
        if (!window.has_value()) {
          return failure(window.error());
        }
        const auto answer =
            window->resize(integer(arguments, "width"), integer(arguments, "height"));
        return answer.has_value() ? changed("window_id", window->id())
                                  : failure(answer.error());
      },
      "Replace one window's dimensions.");

  manage(
      "resize_pane", "Resize a tmux pane",
      {field("paneId", "Stable pane ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("width", "Optional positive pane width.", InputSink::tmux_state, false,
             ArgumentType::integer, 1, 100000),
       field("height", "Optional positive pane height.", InputSink::tmux_state, false,
             ArgumentType::integer, 1, 100000)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto pane = server.pane(required(arguments, "paneId"));
        if (!pane.has_value()) {
          return failure(pane.error());
        }
        if (!arguments.contains("width") && !arguments.contains("height")) {
          return libtmux::unexpected(
              ToolError{true, "resize_pane requires width or height"});
        }
        if (arguments.contains("width")) {
          const auto answer = pane->set_width(integer(arguments, "width"));
          if (!answer.has_value()) {
            return failure(answer.error());
          }
        }
        if (arguments.contains("height")) {
          const auto answer = pane->set_height(integer(arguments, "height"));
          if (!answer.has_value()) {
            return failure(answer.error());
          }
        }
        return changed("pane_id", pane->id());
      },
      "Replace one or both pane dimensions.");

  manage(
      "move_window", "Move a tmux window",
      {field("windowId", "Stable window ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("index", "Non-negative destination index.", InputSink::tmux_state, true,
             ArgumentType::integer, 0, 100000)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto window = server.window(required(arguments, "windowId"));
        if (!window.has_value()) {
          return failure(window.error());
        }
        const auto answer = window->move_to(integer(arguments, "index"));
        return answer.has_value() ? changed("window_id", window->id())
                                  : failure(answer.error());
      },
      "Move a window to an exact index in its owning session.");

  manage(
      "swap_pane", "Swap two tmux panes",
      {field("sourcePaneId", "Stable source pane ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("targetPaneId", "Stable target pane ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto source = server.pane(required(arguments, "sourcePaneId"));
        if (!source.has_value()) {
          return failure(source.error());
        }
        const auto target = server.pane(required(arguments, "targetPaneId"));
        if (!target.has_value()) {
          return failure(target.error());
        }
        const auto answer = source->swap_with(*target);
        return answer.has_value() ? changed("pane_id", source->id())
                                  : failure(answer.error());
      },
      "Exchange two pane positions without changing their IDs.");

  manage(
      "set_pane_title", "Set a tmux pane title",
      {field("paneId", "Stable pane ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("title", "Literal replacement pane title.", InputSink::tmux_format, true,
             ArgumentType::string, {}, {}, 4096U, NestedAuthority::controlled,
             InputControl::double_hash_once)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto pane = server.pane(required(arguments, "paneId"));
        if (!pane.has_value()) {
          return failure(pane.error());
        }
        const auto answer = pane->set_title(required(arguments, "title"));
        return answer.has_value() ? changed("pane_id", pane->id())
                                  : failure(answer.error());
      },
      "Replace one pane title.");

  manage(
      "wait_for_channel", "Wait for a tmux channel",
      {field("channel", "Bounded channel name.", InputSink::tmux_state, true,
             ArgumentType::string, {}, {}, 256U),
       field("timeoutMs", "Optional bounded wait in milliseconds.", InputSink::none,
             false, ArgumentType::integer, 1, 60000)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        std::optional<std::chrono::milliseconds> timeout;
        if (arguments.contains("timeoutMs")) {
          timeout = std::chrono::milliseconds{integer(arguments, "timeoutMs")};
        }
        const auto answer = server.wait_for(required(arguments, "channel"), timeout);
        return answer.has_value()
                   ? detail::output({{"channel", required(arguments, "channel")},
                                     {"signalled", true}})
                   : failure(answer.error());
      },
      "Wait for a named server-side channel with an optional deadline.");

  manage(
      "signal_channel", "Signal a tmux channel",
      {field("channel", "Bounded channel name.", InputSink::tmux_state, true,
             ArgumentType::string, {}, {}, 256U)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto answer = server.signal(required(arguments, "channel"));
        return answer.has_value()
                   ? detail::output({{"channel", required(arguments, "channel")},
                                     {"signalled", true}})
                   : failure(answer.error());
      },
      "Release or latch one named server-side channel.");

  manage(
      "set_mouse_enabled", "Set tmux mouse handling",
      {field("enabled", "Whether mouse handling is enabled.", InputSink::tmux_state,
             true, ArgumentType::boolean)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const bool enabled = boolean(arguments, "enabled");
        const auto answer = server.set_global_option("mouse", enabled ? "on" : "off");
        return answer.has_value() ? detail::output({{"enabled", enabled}})
                                  : failure(answer.error());
      },
      "Replace the global mouse option with a typed boolean.");

  manage(
      "set_history_limit", "Set tmux history limit",
      {field("session", "Stable session ID or name.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("limit", "Retained history line limit.", InputSink::tmux_state, true,
             ArgumentType::integer, 0, 10000000)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto session = server.session(required(arguments, "session"));
        if (!session.has_value()) {
          return failure(session.error());
        }
        const auto answer = session->set_option(
            "history-limit", std::to_string(integer(arguments, "limit")));
        return answer.has_value()
                   ? detail::output({{"limit", integer(arguments, "limit")},
                                     {"session_id", session->id()}})
                   : failure(answer.error());
      },
      "Replace retained-history bounds with a non-negative integer.");
  add(make_tool(
      "create_session", "Create a tmux session", Toolset::execute,
      ProcessReach::configured_process, {Effect::observe, Effect::change},
      {OutputClass::tmux_metadata}, false, false, kConservativeAnnotations,
      {field("height", "Optional detached height.", InputSink::tmux_state, false,
             ArgumentType::integer, 1, 100000),
       field("name", "Optional literal unique session name.", InputSink::tmux_format,
             false, ArgumentType::string, {}, {}, detail::kTargetCharacters,
             NestedAuthority::controlled, InputControl::double_hash_once),
       field("startDirectory", "Literal pane start directory.", InputSink::tmux_format,
             false, ArgumentType::string, {}, {}, 4096U, NestedAuthority::controlled,
             InputControl::double_hash_once),
       field("width", "Optional detached width.", InputSink::tmux_state, false,
             ArgumentType::integer, 1, 100000),
       field("windowName", "Optional literal first window name.",
             InputSink::tmux_format, false, ArgumentType::string, {}, {},
             detail::kTargetCharacters, NestedAuthority::controlled,
             InputControl::double_hash_once)},
      OutputShape::session_id,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        static std::atomic_uint64_t sequence{0U};
        NewSessionOptions options;
        options.name = optional(arguments, "name");
        if (options.name.empty()) {
          options.name = "libtmux-mcp-" + std::to_string(++sequence);
        }
        options.first_window_name = optional(arguments, "windowName");
        options.start_directory = optional(arguments, "startDirectory");
        if (arguments.contains("width")) {
          options.width = static_cast<int>(integer(arguments, "width"));
        }
        if (arguments.contains("height")) {
          options.height = static_cast<int>(integer(arguments, "height"));
        }
        const auto session = server.new_session(std::move(options));
        return session.has_value() ? detail::output({{"name", session->name()},
                                                     {"session_id", session->id()}})
                                   : failure(session.error());
      },
      "Create a detached session; names and path data are literalized before tmux "
      "expansion."));

  add(make_tool(
      "create_window", "Create a tmux window", Toolset::execute,
      ProcessReach::configured_process, {Effect::observe, Effect::change},
      {OutputClass::tmux_metadata}, false, false, kConservativeAnnotations,
      {field("name", "Optional literal window name.", InputSink::tmux_format, false,
             ArgumentType::string, {}, {}, detail::kTargetCharacters,
             NestedAuthority::controlled, InputControl::double_hash_once),
       field("session", "Stable owning session ID or name.", InputSink::tmux_lookup,
             true, ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("startDirectory", "Literal pane start directory.", InputSink::tmux_format,
             false, ArgumentType::string, {}, {}, 4096U, NestedAuthority::controlled,
             InputControl::double_hash_once)},
      OutputShape::window_id,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        static std::atomic_uint64_t sequence{0U};
        const auto session = server.session(required(arguments, "session"));
        if (!session.has_value()) {
          return failure(session.error());
        }
        NewWindowOptions options;
        options.name = optional(arguments, "name");
        if (options.name.empty()) {
          options.name = "libtmux-mcp-" + std::to_string(++sequence);
        }
        options.start_directory = optional(arguments, "startDirectory");
        const auto window = session->new_window(std::move(options));
        return window.has_value()
                   ? detail::output({{"session_id", window->session_id()},
                                     {"window_id", window->id()}})
                   : failure(window.error());
      },
      "Create a detached window; name and path data are literalized before tmux "
      "expansion."));

  add(make_tool(
      "split_window", "Split a tmux window", Toolset::execute,
      ProcessReach::configured_process, {Effect::observe, Effect::change},
      {OutputClass::tmux_metadata}, false, false, kConservativeAnnotations,
      {field("direction", "vertical or horizontal.", InputSink::tmux_state, false,
             ArgumentType::string, {}, {}, 16U),
       field("paneId", "Stable pane to split.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("startDirectory", "Literal pane start directory.", InputSink::tmux_format,
             false, ArgumentType::string, {}, {}, 4096U, NestedAuthority::controlled,
             InputControl::double_hash_once)},
      OutputShape::pane_id,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto pane = server.pane(required(arguments, "paneId"));
        if (!pane.has_value()) {
          return failure(pane.error());
        }
        const std::string direction = optional(arguments, "direction");
        if (!direction.empty() && direction != "vertical" &&
            direction != "horizontal") {
          return libtmux::unexpected(
              ToolError{true, "direction must be vertical or horizontal"});
        }
        SplitOptions options;
        options.horizontal = direction == "horizontal";
        options.start_directory = optional(arguments, "startDirectory");
        const auto created = pane->split(std::move(options));
        if (!created.has_value()) {
          return failure(created.error());
        }
        return detail::output({{"pane_id", created->id()}});
      },
      "Create a configured-process pane; path data is literalized before tmux "
      "expansion."));

  add(make_tool(
      "respawn_pane", "Respawn a tmux pane", Toolset::execute,
      ProcessReach::configured_process,
      {Effect::observe, Effect::change, Effect::delete_}, {OutputClass::tmux_metadata},
      false, false, kConservativeAnnotations,
      {field("force", "Permit replacing a running pane process.", InputSink::none,
             false, ArgumentType::boolean),
       field("killFirst", "Kill a running pane process before respawn.",
             InputSink::none, false, ArgumentType::boolean),
       field("paneId", "Stable pane ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("startDirectory", "Literal replacement start directory.",
             InputSink::tmux_format, false, ArgumentType::string, {}, {}, 4096U,
             NestedAuthority::controlled, InputControl::double_hash_once)},
      OutputShape::pane_id,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto pane = server.pane(required(arguments, "paneId"));
        if (!pane.has_value()) {
          return failure(pane.error());
        }
        RespawnOptions options;
        options.replace_running =
            boolean(arguments, "force") || boolean(arguments, "killFirst");
        options.start_directory = optional(arguments, "startDirectory");
        const auto answer = pane->respawn(std::move(options));
        return answer.has_value() ? changed("pane_id", pane->id())
                                  : failure(answer.error());
      },
      "Restart only the pane's configured process; no caller command is accepted."));

  add(make_tool(
      "run_shell_command", "Run a shell command in a tmux pane", Toolset::execute,
      ProcessReach::pane_command, {Effect::observe, Effect::change},
      {OutputClass::tmux_metadata, OutputClass::terminal_content}, true, true,
      kConservativeAnnotations,
      {field("paneId", "Stable pane ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("command", "Shell command sent to the pane.", InputSink::shell_command,
             true, ArgumentType::string, {}, {}, 1024U * 1024U,
             NestedAuthority::unrestricted),
       field("timeoutMs", "Completion timeout in milliseconds.", InputSink::none, false,
             ArgumentType::integer, 1, 60000)},
      OutputShape::object,
      [](const Server& server, const Arguments& arguments,
         const CallContext& context) -> ToolResult {
        static std::atomic_uint64_t sequence{0U};
        const auto pane = server.pane(required(arguments, "paneId"));
        if (!pane.has_value()) {
          return failure(pane.error());
        }
        const std::string marker =
            "__LIBTMUX_MCP_DONE_" + std::to_string(++sequence) + "__";
        std::string payload = required(arguments, "command");
        payload += "; __libtmux_mcp_status=$?; printf '\\n" + marker +
                   ":%s\\n' \"$__libtmux_mcp_status\"\n";
        const auto sent = pane->send_text(payload);
        if (!sent.has_value()) {
          return failure(sent.error());
        }
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds{integer(arguments, "timeoutMs", 30000)};
        while (std::chrono::steady_clock::now() < deadline) {
          if (context.cancelled()) {
            return libtmux::unexpected(detail::cancelled());
          }
          CaptureOptions options;
          options.whole_history = true;
          const auto captured = pane->capture(options);
          if (!captured.has_value()) {
            return failure(captured.error());
          }
          const auto found = captured->rfind(marker + ":");
          if (found != std::string::npos) {
            const std::size_t status_begin = found + marker.size() + 1U;
            const std::size_t status_end = captured->find('\n', status_begin);
            long long status = 0;
            static_cast<void>(std::from_chars(
                captured->data() + status_begin,
                captured->data() +
                    (status_end == std::string::npos ? captured->size() : status_end),
                status));
            return detail::output({{"exit_code", status},
                                   {"pane_id", pane->id()},
                                   {"text", captured->substr(0U, found)}});
          }
          std::this_thread::sleep_for(20ms);
        }
        return libtmux::unexpected(ToolError{false, "shell command timed out"});
      },
      "Send one command, wait for a private completion marker, and return captured "
      "output."));

  add(make_tool(
      "send_keys", "Send keys to a tmux pane", Toolset::execute,
      ProcessReach::pane_input, {Effect::observe, Effect::change},
      {OutputClass::tmux_metadata}, false, true, kConservativeAnnotations,
      {field("paneId", "Stable pane ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("keys", "One tmux key name.", InputSink::pane_input, true,
             ArgumentType::string, {}, {}, 256U, NestedAuthority::unrestricted)},
      OutputShape::pane_targets,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto pane = server.pane(required(arguments, "paneId"));
        if (!pane.has_value()) {
          return failure(pane.error());
        }
        auto targets = resolved_pane_targets(*pane);
        if (!targets.has_value()) {
          return failure(targets.error());
        }
        const auto answer = pane->send_key(required(arguments, "keys"));
        return answer.has_value()
                   ? detail::output(
                         {{"pane_id", pane->id()},
                          {"target_pane_ids", StructuredValue{std::move(*targets)}}})
                   : failure(answer.error());
      },
      "Deliver one validated tmux key name."));

  add(make_tool(
      "send_keys_batch", "Send a key sequence to a tmux pane", Toolset::execute,
      ProcessReach::pane_input, {Effect::observe, Effect::change},
      {OutputClass::tmux_metadata}, false, true, kConservativeAnnotations,
      {field("onError", "Stop after the first failed operation or continue.",
             InputSink::none, false, ArgumentType::string, {}, {}, {},
             NestedAuthority::none, InputControl::none, {"continue", "stop"}),
       send_key_operations_field()},
      OutputShape::object,
      [](const Server& server, const Arguments& arguments,
         const CallContext& context) -> ToolResult {
        if (arguments.send_key_operations.empty() ||
            arguments.send_key_operations.size() > 64U) {
          return libtmux::unexpected(ToolError{
              true, "operations must contain between one and sixty-four operations"});
        }
        std::string on_error = optional(arguments, "onError");
        if (on_error.empty()) {
          on_error = "stop";
        }
        if (on_error != "stop" && on_error != "continue") {
          return libtmux::unexpected(
              ToolError{true, "onError must be stop or continue"});
        }
        StructuredValue::Array failures;
        StructuredValue::Array targets;
        std::size_t completed = 0U;
        for (std::size_t index = 0U; index < arguments.send_key_operations.size();
             ++index) {
          if (context.cancelled()) {
            return libtmux::unexpected(detail::cancelled());
          }
          const FlatArguments& operation = arguments.send_key_operations[index];
          std::string error;
          const auto pane = server.pane(required(operation, "paneId"));
          StructuredValue::Array resolved;
          if (!pane.has_value()) {
            error = pane.error().diagnostic;
          } else {
            auto found_targets = resolved_pane_targets(*pane);
            if (!found_targets.has_value()) {
              error = found_targets.error().diagnostic;
            } else {
              resolved = std::move(*found_targets);
              const auto sent = boolean(operation, "literal")
                                    ? pane->send_text(required(operation, "keys"))
                                    : pane->send_key(required(operation, "keys"));
              if (!sent.has_value()) {
                error = sent.error().diagnostic;
              } else if (boolean(operation, "enter")) {
                const auto entered = pane->send_key("Enter");
                if (!entered.has_value()) {
                  error = entered.error().diagnostic;
                }
              }
            }
          }
          if (!error.empty()) {
            failures.push_back(StructuredValue::Object{
                {"index", static_cast<long long>(index)}, {"reason", error}});
            if (on_error == "stop") {
              break;
            }
            continue;
          }
          ++completed;
          targets.push_back(StructuredValue::Object{
              {"index", static_cast<long long>(index)},
              {"resolvedPaneIds", StructuredValue{std::move(resolved)}}});
        }
        return detail::output({{"completed", static_cast<long long>(completed)},
                               {"failures", StructuredValue{std::move(failures)}},
                               {"targets", StructuredValue{std::move(targets)}}});
      },
      "Deliver an ordered, bounded sequence of pane-input operations."));

  add(make_tool(
      "paste_text", "Paste text into a tmux pane", Toolset::execute,
      ProcessReach::pane_input, {Effect::observe, Effect::change},
      {OutputClass::tmux_metadata}, false, true, kConservativeAnnotations,
      {field("paneId", "Stable pane ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("text", "Literal text to paste.", InputSink::pane_input, true,
             ArgumentType::string, {}, {}, 1024U * 1024U,
             NestedAuthority::unrestricted)},
      OutputShape::pane_id,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        static std::atomic_uint64_t sequence{0U};
        const auto pane = server.pane(required(arguments, "paneId"));
        if (!pane.has_value()) {
          return failure(pane.error());
        }
        const std::string name = "libtmux-mcp-" + std::to_string(++sequence);
        const auto staged = server.set_buffer(name, required(arguments, "text"));
        if (!staged.has_value()) {
          return failure(staged.error());
        }
        const auto buffers = server.buffers();
        if (!buffers.has_value()) {
          return failure(buffers.error());
        }
        const auto buffer = std::ranges::find(*buffers, name, &Buffer::name);
        if (buffer == buffers->end()) {
          return libtmux::unexpected(
              ToolError{false, "temporary paste buffer disappeared"});
        }
        const auto answer = pane->paste(*buffer, true);
        if (!answer.has_value()) {
          static_cast<void>(buffer->remove());
          return failure(answer.error());
        }
        return changed("pane_id", pane->id());
      },
      "Stage a private buffer, paste it once, and consume it."));

  add(make_tool(
      "set_synchronize_panes", "Set synchronized pane input", Toolset::execute,
      ProcessReach::none, {Effect::change}, {OutputClass::tmux_metadata}, false, false,
      kConservativeAnnotations,
      {field("windowId", "Stable window ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("enabled", "Whether later pane input is duplicated.",
             InputSink::tmux_state, true, ArgumentType::boolean)},
      OutputShape::object,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto window = server.window(required(arguments, "windowId"));
        if (!window.has_value()) {
          return failure(window.error());
        }
        const bool enabled = boolean(arguments, "enabled");
        const auto answer =
            window->set_option("synchronize-panes", enabled ? "on" : "off");
        return answer.has_value()
                   ? detail::output({{"enabled", enabled}, {"window_id", window->id()}})
                   : failure(answer.error());
      },
      "Enabling synchronize-panes means subsequent input is copied to every pane "
      "in the window.",
      true));
  const auto teardown = [&](std::string name, std::string title,
                            std::vector<Field> fields, Handler handler,
                            std::string description) {
    const bool observes = name != "clear_pane_scrollback";
    add(make_tool(std::move(name), std::move(title), Toolset::teardown,
                  ProcessReach::none,
                  observes ? std::set{Effect::observe, Effect::delete_}
                           : std::set{Effect::delete_},
                  {OutputClass::tmux_metadata}, false, false, kConservativeAnnotations,
                  std::move(fields), OutputShape::object, std::move(handler),
                  std::move(description)));
  };

  teardown(
      "clear_pane_scrollback", "Clear tmux pane scrollback",
      {field("paneId", "Stable pane ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto pane = server.pane(required(arguments, "paneId"));
        if (!pane.has_value()) {
          return failure(pane.error());
        }
        const auto answer = pane->clear_history();
        return answer.has_value() ? changed("pane_id", pane->id())
                                  : failure(answer.error());
      },
      "Irreversibly discard retained scrollback for one pane.");

  teardown(
      "kill_pane", "Kill a tmux pane",
      {field("paneId", "Stable pane ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto pane = server.pane(required(arguments, "paneId"));
        if (!pane.has_value()) {
          return failure(pane.error());
        }
        const std::string id{pane->id()};
        const auto answer = pane->kill();
        return answer.has_value() ? detail::output({{"pane_id", id}, {"deleted", true}})
                                  : failure(answer.error());
      },
      "Delete one pane and its running process.");

  teardown(
      "kill_window", "Kill a tmux window",
      {field("windowId", "Stable window ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto window = server.window(required(arguments, "windowId"));
        if (!window.has_value()) {
          return failure(window.error());
        }
        const std::string id{window->id()};
        const auto answer = window->kill();
        return answer.has_value()
                   ? detail::output({{"window_id", id}, {"deleted", true}})
                   : failure(answer.error());
      },
      "Delete one window and every pane it owns.");

  teardown(
      "kill_session", "Kill a tmux session",
      {field("session", "Stable session ID or name.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto session = server.session(required(arguments, "session"));
        if (!session.has_value()) {
          return failure(session.error());
        }
        const std::string id{session->id()};
        const auto answer = session->kill();
        return answer.has_value()
                   ? detail::output({{"session_id", id}, {"deleted", true}})
                   : failure(answer.error());
      },
      "Delete one session and every window it owns.");
  return tools;
}

} // namespace

libtmux::expected<ToolRegistry, std::string> default_tools() {
  return default_tools(ToolSelection::all());
}

libtmux::expected<ToolRegistry, std::string>
default_tools(const ToolSelection& selection) {
  return ToolRegistry::create(definitions(), selection);
}

} // namespace libtmux::mcp
