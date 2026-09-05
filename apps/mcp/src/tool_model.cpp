#include "libtmux_consumers/mcp.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtmux/server.hpp"
#include "tool_support.hpp"

namespace libtmux::mcp {
namespace {

[[nodiscard]] const std::string* argument(const FlatArguments& arguments,
                                          std::string_view name) {
  const auto found = arguments.find(name);
  return found == arguments.end() ? nullptr : &found->second;
}

[[nodiscard]] libtmux::expected<long long, std::string>
parse_integer(std::string_view text, const Parameter& parameter) {
  long long value = 0;
  const char* const end = text.data() + text.size();
  const auto [stopped, code] = std::from_chars(text.data(), end, value);
  if (code != std::errc{} || stopped != end) {
    return libtmux::unexpected(parameter.name + " must be an integer");
  }
  if (parameter.minimum.has_value() && value < *parameter.minimum) {
    return libtmux::unexpected(parameter.name + " must be at least " +
                               std::to_string(*parameter.minimum));
  }
  if (parameter.maximum.has_value() && value > *parameter.maximum) {
    return libtmux::unexpected(parameter.name + " must be at most " +
                               std::to_string(*parameter.maximum));
  }
  return value;
}

[[nodiscard]] std::optional<std::size_t>
utf8_code_points(std::string_view value) noexcept {
  std::size_t count = 0;
  std::size_t offset = 0;
  while (offset < value.size()) {
    const auto lead = static_cast<unsigned char>(value[offset]);
    std::size_t width = 0;
    if (lead <= 0x7FU) {
      width = 1U;
    } else if (lead >= 0xC2U && lead <= 0xDFU) {
      width = 2U;
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
      width = 3U;
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
      width = 4U;
    } else {
      return std::nullopt;
    }
    if (width > value.size() - offset) {
      return std::nullopt;
    }
    for (std::size_t index = 1; index < width; ++index) {
      const auto byte = static_cast<unsigned char>(value[offset + index]);
      if (byte < 0x80U || byte > 0xBFU) {
        return std::nullopt;
      }
    }
    if (width >= 3U) {
      const auto second = static_cast<unsigned char>(value[offset + 1U]);
      if ((lead == 0xE0U && second < 0xA0U) || (lead == 0xEDU && second > 0x9FU) ||
          (lead == 0xF0U && second < 0x90U) || (lead == 0xF4U && second > 0x8FU)) {
        return std::nullopt;
      }
    }
    offset += width;
    ++count;
  }
  return count;
}

[[nodiscard]] std::optional<ToolError>
validate_send_key_operations(const std::vector<FlatArguments>& operations) {
  for (std::size_t index = 0U; index < operations.size(); ++index) {
    const FlatArguments& operation = operations[index];
    const std::string prefix = "operations[" + std::to_string(index) + "]";
    if (!operation.string_arrays.empty()) {
      return ToolError{true, prefix + " must be a closed input operation"};
    }
    for (const auto& [name, value] : operation) {
      static_cast<void>(value);
      if (name != "paneId" && name != "keys" && name != "enter" && name != "literal") {
        return ToolError{true, "unknown argument: " + prefix + "." + name};
      }
    }
    for (const std::string_view name : {"paneId", "keys"}) {
      const std::string* const value = argument(operation, name);
      if (value == nullptr || value->empty()) {
        return ToolError{true, prefix + "." + std::string{name} +
                                   " must be a non-empty string"};
      }
      const auto length = utf8_code_points(*value);
      if (!length.has_value()) {
        return ToolError{true, prefix + "." + std::string{name} +
                                   " must contain valid UTF-8"};
      }
      const std::size_t maximum =
          name == "paneId" ? detail::kTargetCharacters : detail::kSearchCharacters;
      if (*length > maximum) {
        return ToolError{true, prefix + "." + std::string{name} + " is longer than " +
                                   std::to_string(maximum) + " characters"};
      }
    }
    for (const std::string_view name : {"enter", "literal"}) {
      const std::string* const value = argument(operation, name);
      if (value != nullptr && *value != "true" && *value != "false") {
        return ToolError{true, prefix + "." + std::string{name} + " must be a boolean"};
      }
    }
  }
  return std::nullopt;
}

[[nodiscard]] bool valid(Toolset value) noexcept {
  switch (value) {
  case Toolset::inspect:
  case Toolset::manage:
  case Toolset::execute:
  case Toolset::teardown:
    return true;
  }
  return false;
}

[[nodiscard]] bool valid(ArgumentType value) noexcept {
  switch (value) {
  case ArgumentType::string:
  case ArgumentType::integer:
  case ArgumentType::boolean:
  case ArgumentType::string_array:
  case ArgumentType::read_calls:
  case ArgumentType::send_key_operations:
    return true;
  }
  return false;
}

[[nodiscard]] bool valid(OutputShape value) noexcept {
  switch (value) {
  case OutputShape::overview:
  case OutputShape::sessions:
  case OutputShape::windows:
  case OutputShape::panes:
  case OutputShape::pane_text:
  case OutputShape::pane_id:
  case OutputShape::pane_targets:
  case OutputShape::session_id:
  case OutputShape::window_id:
  case OutputShape::wait:
  case OutputShape::matches:
  case OutputShape::object:
    return true;
  }
  return false;
}

[[nodiscard]] bool valid(ProcessReach value) noexcept {
  switch (value) {
  case ProcessReach::none:
  case ProcessReach::configured_process:
  case ProcessReach::pane_input:
  case ProcessReach::pane_command:
    return true;
  }
  return false;
}

[[nodiscard]] bool valid(Effect value) noexcept {
  switch (value) {
  case Effect::observe:
  case Effect::change:
  case Effect::delete_:
    return true;
  }
  return false;
}

[[nodiscard]] bool valid(OutputClass value) noexcept {
  switch (value) {
  case OutputClass::tmux_metadata:
  case OutputClass::terminal_content:
  case OutputClass::process_environment:
  case OutputClass::configured_command:
    return true;
  }
  return false;
}

[[nodiscard]] bool valid(InputSink value) noexcept {
  switch (value) {
  case InputSink::none:
  case InputSink::tmux_lookup:
  case InputSink::tmux_state:
  case InputSink::pane_input:
  case InputSink::shell_command:
  case InputSink::process_argv:
  case InputSink::regex:
  case InputSink::tmux_format:
  case InputSink::nested_tool:
    return true;
  }
  return false;
}

[[nodiscard]] bool valid(InputControl value) noexcept {
  switch (value) {
  case InputControl::none:
  case InputControl::double_hash_once:
  case InputControl::validated_variable_name:
    return true;
  }
  return false;
}

[[nodiscard]] bool valid(NestedAuthority value) noexcept {
  switch (value) {
  case NestedAuthority::none:
  case NestedAuthority::controlled:
  case NestedAuthority::unrestricted:
    return true;
  }
  return false;
}

[[nodiscard]] std::string_view controlled_opener(const ToolDefinition& tool) {
  switch (tool.authority.process_reach) {
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
  if (tool.toolset == Toolset::teardown) {
    return "Delete tmux state; accepts no command payload.";
  }
  if (tool.authority.output_classes.contains(OutputClass::terminal_content)) {
    return "Read pane output; accepts no client-supplied executable input. Returned "
           "content may be sensitive or untrusted.";
  }
  if (tool.authority.output_classes.contains(OutputClass::process_environment)) {
    return "Read the tmux environment; accepts no client-supplied executable input. "
           "Returned values may contain secrets.";
  }
  if (tool.authority.output_classes.contains(OutputClass::configured_command)) {
    return "Read configured tmux commands; accepts no client-supplied executable "
           "input. Returned values may contain executable configuration.";
  }
  if (tool.authority.effects.contains(Effect::change)) {
    return "Change tmux state; no client-supplied executable input.";
  }
  return "Inspect tmux metadata; accepts no client-supplied executable input.";
}

[[nodiscard]] libtmux::expected<void, std::string>
validate_definition(const ToolDefinition& tool) {
  if (tool.name.empty()) {
    return libtmux::unexpected(std::string{"tool name is empty"});
  }
  if (tool.title.empty()) {
    return libtmux::unexpected("tool title is empty: " + tool.name);
  }
  if (!valid(tool.toolset)) {
    return libtmux::unexpected("invalid toolset: " + tool.name);
  }
  if (!valid(tool.authority.process_reach)) {
    return libtmux::unexpected("invalid process reach: " + tool.name);
  }
  if (tool.authority.effects.empty()) {
    return libtmux::unexpected("effects must not be empty: " + tool.name);
  }
  if (!std::ranges::all_of(tool.authority.effects,
                           [](Effect effect) { return valid(effect); })) {
    return libtmux::unexpected("invalid effect: " + tool.name);
  }
  const Effect toolset_effect =
      tool.toolset == Toolset::inspect
          ? Effect::observe
          : (tool.toolset == Toolset::teardown ? Effect::delete_ : Effect::change);
  if (!tool.authority.effects.contains(toolset_effect)) {
    return libtmux::unexpected("toolset/effect mismatch: " + tool.name);
  }
  if (tool.toolset == Toolset::manage &&
      tool.authority.effects.contains(Effect::delete_)) {
    return libtmux::unexpected("manage tool has a delete effect: " + tool.name);
  }
  if (tool.authority.process_reach != ProcessReach::none &&
      !tool.authority.effects.contains(Effect::change)) {
    return libtmux::unexpected("process reach requires change effect: " + tool.name);
  }
  if ((tool.toolset == Toolset::inspect || tool.toolset == Toolset::teardown) &&
      tool.authority.process_reach != ProcessReach::none) {
    return libtmux::unexpected("toolset/process reach mismatch: " + tool.name);
  }
  if ((tool.authority.process_reach == ProcessReach::pane_input ||
       tool.authority.process_reach == ProcessReach::pane_command) &&
      tool.toolset != Toolset::execute) {
    return libtmux::unexpected("pane reach requires execute toolset: " + tool.name);
  }
  if (tool.authority.output_classes.empty()) {
    return libtmux::unexpected("output classes must not be empty: " + tool.name);
  }
  if (!std::ranges::all_of(tool.authority.output_classes,
                           [](OutputClass output) { return valid(output); })) {
    return libtmux::unexpected("invalid output class: " + tool.name);
  }
  if (tool.authority.output_classes.contains(OutputClass::terminal_content) &&
      (!tool.authority.may_expose_secrets ||
       !tool.authority.may_return_untrusted_content)) {
    return libtmux::unexpected("terminal-content disclosure mismatch: " + tool.name);
  }
  if (tool.authority.output_classes.contains(OutputClass::process_environment) &&
      !tool.authority.may_expose_secrets) {
    return libtmux::unexpected("process-environment disclosure mismatch: " + tool.name);
  }
  if (tool.authority.output_classes.contains(OutputClass::configured_command) &&
      !tool.authority.may_return_untrusted_content) {
    return libtmux::unexpected("configured-command disclosure mismatch: " + tool.name);
  }
  if (tool.authority.amplifies_future_input != (tool.name == "set_synchronize_panes")) {
    return libtmux::unexpected("future-input amplification mismatch: " + tool.name);
  }
  if (!tool.handler) {
    return libtmux::unexpected("handler is missing: " + tool.name);
  }
  const bool read_batch = tool.name == "call_read_tools_batch";
  if (read_batch != (tool.authority.nested_tools.size() == 16U)) {
    return libtmux::unexpected(
        "read batch must declare exactly sixteen nested tools: " + tool.name);
  }
  if (tool.authority.nested_tools.contains(tool.name)) {
    return libtmux::unexpected("tool cannot declare itself as nested authority: " +
                               tool.name);
  }

  if (tool.annotations != kConservativeAnnotations) {
    return libtmux::unexpected("annotations must be conservative: " + tool.name);
  }

  std::set<std::string, std::less<>> schema_names;
  if (!valid(tool.schema.output)) {
    return libtmux::unexpected("invalid output schema: " + tool.name);
  }
  for (const Parameter& parameter : tool.schema.input) {
    if (parameter.name.empty()) {
      return libtmux::unexpected("schema field is empty: " + tool.name);
    }
    if (!schema_names.insert(parameter.name).second) {
      return libtmux::unexpected("duplicate schema field: " + parameter.name);
    }
    if (!valid(parameter.type)) {
      return libtmux::unexpected("invalid schema field type: " + parameter.name);
    }
    if (!parameter.allowed_values.empty()) {
      if (parameter.type != ArgumentType::string ||
          std::ranges::any_of(parameter.allowed_values,
                              [](const std::string& value) { return value.empty(); }) ||
          std::set<std::string, std::less<>>{parameter.allowed_values.begin(),
                                             parameter.allowed_values.end()}
                  .size() != parameter.allowed_values.size()) {
        return libtmux::unexpected("invalid schema field enum: " + parameter.name);
      }
    }
    if (!tool.authority.input_sinks.contains(parameter.name)) {
      return libtmux::unexpected("missing input sinks: " + parameter.name);
    }
  }

  bool pane_input = false;
  bool shell_command = false;
  bool process_argv = false;
  for (const auto& [name, sinks] : tool.authority.input_sinks) {
    if (!schema_names.contains(name)) {
      return libtmux::unexpected("extra input sinks: " + name);
    }
    if (sinks.empty()) {
      return libtmux::unexpected("input sink set must not be empty: " + name);
    }
    if (sinks.size() > 1U && std::ranges::any_of(sinks, [](const Sink& sink) {
          return sink.type == InputSink::none;
        })) {
      return libtmux::unexpected("none must be the only input sink: " + name);
    }
    for (const Sink& sink : sinks) {
      if (!valid(sink.type)) {
        return libtmux::unexpected("invalid input sink: " + name);
      }
      if (!valid(sink.nested_authority)) {
        return libtmux::unexpected("invalid nested authority: " + name);
      }
      if (!valid(sink.control)) {
        return libtmux::unexpected("invalid input control: " + name);
      }
      const bool executable_sink =
          sink.type == InputSink::pane_input || sink.type == InputSink::shell_command ||
          sink.type == InputSink::process_argv || sink.type == InputSink::tmux_format;
      const bool nested_sink = sink.type == InputSink::nested_tool;
      if (!executable_sink && !nested_sink && sink.type != InputSink::none &&
          sink.nested_authority != NestedAuthority::none) {
        return libtmux::unexpected("non-executable sink has nested authority: " + name);
      }
      if (executable_sink && sink.nested_authority == NestedAuthority::none) {
        return libtmux::unexpected("executable sink lacks nested authority: " + name);
      }
      if (nested_sink && sink.nested_authority == NestedAuthority::none) {
        return libtmux::unexpected("nested tool sink lacks authority: " + name);
      }
      if (nested_sink && sink.nested_authority != NestedAuthority::controlled) {
        return libtmux::unexpected("nested tool sink must be controlled: " + name);
      }
      if (sink.type == InputSink::tmux_format &&
          sink.nested_authority == NestedAuthority::unrestricted) {
        return libtmux::unexpected("unrestricted tmux-format is prohibited: " + name);
      }
      if (sink.type == InputSink::tmux_format && sink.control == InputControl::none) {
        return libtmux::unexpected("tmux-format lacks a literalization control: " +
                                   name);
      }
      if (sink.type != InputSink::tmux_format && sink.control != InputControl::none) {
        return libtmux::unexpected("non-format sink has a format control: " + name);
      }
      pane_input = pane_input || sink.type == InputSink::pane_input;
      shell_command = shell_command || sink.type == InputSink::shell_command;
      process_argv = process_argv || sink.type == InputSink::process_argv;
    }
  }

  if (tool.authority.process_reach == ProcessReach::configured_process &&
      (pane_input || shell_command || process_argv)) {
    return libtmux::unexpected("configured-process accepts no executable input sink: " +
                               tool.name);
  }
  if ((tool.authority.process_reach == ProcessReach::pane_input) != pane_input) {
    return libtmux::unexpected("pane-input reach/sink mismatch: " + tool.name);
  }
  if ((tool.authority.process_reach == ProcessReach::pane_command) != shell_command) {
    return libtmux::unexpected("pane-command reach/sink mismatch: " + tool.name);
  }
  if (process_argv) {
    return libtmux::unexpected("process-argv input is prohibited: " + tool.name);
  }
  if (!tool.description.starts_with(controlled_opener(tool))) {
    return libtmux::unexpected("description has the wrong controlled opener: " +
                               tool.name);
  }
  return {};
}

[[nodiscard]] std::string_view trim(std::string_view value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string_view::npos) {
    return {};
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1U);
}

[[nodiscard]] libtmux::expected<std::vector<std::string>, std::string>
parse_names(std::optional<std::string_view> value, std::string_view variable,
            bool whole_empty_is_valid) {
  std::vector<std::string> names;
  if (!value.has_value()) {
    return names;
  }
  if (value->empty()) {
    if (whole_empty_is_valid) {
      return names;
    }
    return libtmux::unexpected(std::string{variable} + " contains an empty name");
  }
  std::size_t offset = 0;
  while (offset <= value->size()) {
    const auto comma = value->find(',', offset);
    const std::string_view name = trim(
        value->substr(offset, comma == std::string_view::npos ? std::string_view::npos
                                                              : comma - offset));
    if (name.empty()) {
      return libtmux::unexpected(std::string{variable} + " contains an empty name");
    }
    names.emplace_back(name);
    if (comma == std::string_view::npos) {
      break;
    }
    offset = comma + 1U;
  }
  return names;
}

[[nodiscard]] std::optional<std::string_view> environment(std::string_view name) {
  const char* const value = std::getenv(std::string{name}.c_str());
  return value == nullptr ? std::nullopt
                          : std::optional<std::string_view>{std::string_view{value}};
}

} // namespace

std::vector<std::string> ToolDefinition::required_names() const {
  std::vector<std::string> names;
  for (const Parameter& parameter : schema.input) {
    if (parameter.required) {
      names.push_back(parameter.name);
    }
  }
  return names;
}

ToolSelection ToolSelection::all() {
  return ToolSelection{.toolsets = {Toolset::inspect, Toolset::manage, Toolset::execute,
                                    Toolset::teardown},
                       .include = {},
                       .exclude = {}};
}

libtmux::expected<ToolRegistry, std::string>
ToolRegistry::create(std::vector<ToolDefinition> definitions,
                     const ToolSelection& selection) {
  for (const Toolset toolset : selection.toolsets) {
    if (!valid(toolset)) {
      return libtmux::unexpected(std::string{"invalid selected toolset"});
    }
  }
  std::set<std::string, std::less<>> names;
  for (const ToolDefinition& definition : definitions) {
    if (auto valid_definition = validate_definition(definition);
        !valid_definition.has_value()) {
      return libtmux::unexpected(valid_definition.error());
    }
    if (!names.insert(definition.name).second) {
      return libtmux::unexpected("duplicate tool: " + definition.name);
    }
  }
  for (const ToolDefinition& definition : definitions) {
    for (const std::string& nested_name : definition.authority.nested_tools) {
      const auto nested =
          std::ranges::find(definitions, nested_name, &ToolDefinition::name);
      if (nested == definitions.end() || nested->toolset != Toolset::inspect) {
        return libtmux::unexpected("invalid nested tool: " + nested_name);
      }
    }
  }
  for (const std::string& name : selection.include) {
    if (!names.contains(name)) {
      return libtmux::unexpected("unknown included tool: " + name);
    }
  }
  for (const std::string& name : selection.exclude) {
    if (!names.contains(name)) {
      return libtmux::unexpected("unknown excluded tool: " + name);
    }
  }
  const auto selected = [&selection](const ToolDefinition& definition) {
    return (selection.toolsets.contains(definition.toolset) ||
            selection.include.contains(definition.name)) &&
           !selection.exclude.contains(definition.name);
  };
  const auto batch =
      std::ranges::find(definitions, "call_read_tools_batch", &ToolDefinition::name);
  const bool batch_visible = batch != definitions.end() && selected(*batch);
  const std::set<std::string, std::less<>> nested_names =
      batch == definitions.end() ? std::set<std::string, std::less<>>{}
                                 : batch->authority.nested_tools;
  if (batch != definitions.end()) {
    std::erase_if(batch->authority.nested_tools,
                  [&selection](const std::string& nested_name) {
                    return selection.exclude.contains(nested_name);
                  });
    if (batch_visible) {
      batch->authority.effects.clear();
      batch->authority.output_classes.clear();
      batch->authority.may_expose_secrets = false;
      batch->authority.may_return_untrusted_content = false;
      for (const std::string& nested_name : batch->authority.nested_tools) {
        const auto nested =
            std::ranges::find(definitions, nested_name, &ToolDefinition::name);
        if (nested == definitions.end()) {
          continue;
        }
        batch->authority.effects.insert(nested->authority.effects.begin(),
                                        nested->authority.effects.end());
        batch->authority.output_classes.insert(nested->authority.output_classes.begin(),
                                               nested->authority.output_classes.end());
        batch->authority.may_expose_secrets |= nested->authority.may_expose_secrets;
        batch->authority.may_return_untrusted_content |=
            nested->authority.may_return_untrusted_content;
      }
      if (batch->authority.effects.empty()) {
        batch->authority.effects.insert(Effect::observe);
      }
    }
  }
  std::vector<ToolDefinition> visible;
  std::vector<ToolDefinition> hidden;
  visible.reserve(definitions.size());
  hidden.reserve(nested_names.size());
  for (ToolDefinition& definition : definitions) {
    if (selected(definition)) {
      visible.push_back(std::move(definition));
    } else if (batch_visible && nested_names.contains(definition.name) &&
               !selection.exclude.contains(definition.name)) {
      hidden.push_back(std::move(definition));
    }
  }
  const std::size_t visible_count = visible.size();
  visible.insert(visible.end(), std::make_move_iterator(hidden.begin()),
                 std::make_move_iterator(hidden.end()));
  return ToolRegistry{std::move(visible), visible_count, selection};
}

std::span<const ToolDefinition> ToolRegistry::tools() const noexcept {
  return {definitions_.data(), visible_count_};
}

const ToolSelection& ToolRegistry::selection() const noexcept { return selection_; }

const ToolDefinition* ToolRegistry::find(std::string_view name) const noexcept {
  const auto visible = tools();
  const auto found = std::ranges::find(visible, name, &ToolDefinition::name);
  return found == visible.end() ? nullptr : &*found;
}

const ToolDefinition* ToolRegistry::find_any(std::string_view name) const noexcept {
  const auto found = std::ranges::find(definitions_, name, &ToolDefinition::name);
  return found == definitions_.end() ? nullptr : &*found;
}

const ToolDefinition* ToolRegistry::find_nested(const ToolDefinition& caller,
                                                std::string_view name) const noexcept {
  return caller.authority.nested_tools.contains(name) ? find_any(name) : nullptr;
}

ToolResult ToolRegistry::call(const Server& server, std::string_view name,
                              const Arguments& arguments,
                              const CallContext& context) const {
  const ToolDefinition* const tool = find(name);
  if (tool == nullptr) {
    return libtmux::unexpected(ToolError{true, "unknown tool: " + std::string{name}});
  }
  return call_definition(server, *tool, arguments, context);
}

ToolResult ToolRegistry::call_definition(const Server& server,
                                         const ToolDefinition& tool,
                                         const Arguments& arguments,
                                         const CallContext& context) const {
  const auto read_calls =
      std::ranges::find(tool.schema.input, ArgumentType::read_calls, &Parameter::type);
  if (!arguments.read_calls.empty() && read_calls == tool.schema.input.end()) {
    return libtmux::unexpected(ToolError{true, "unknown argument: operations"});
  }
  if (arguments.read_calls.size() > 16U) {
    return libtmux::unexpected(
        ToolError{true, "operations must contain at most sixteen operations"});
  }
  const auto send_operations = std::ranges::find(
      tool.schema.input, ArgumentType::send_key_operations, &Parameter::type);
  if (!arguments.send_key_operations.empty() &&
      send_operations == tool.schema.input.end()) {
    return libtmux::unexpected(ToolError{true, "unknown argument: operations"});
  }
  if (arguments.send_key_operations.size() > 64U) {
    return libtmux::unexpected(
        ToolError{true, "operations must contain at most sixty-four operations"});
  }
  if (const auto invalid = validate_send_key_operations(arguments.send_key_operations);
      invalid.has_value()) {
    return libtmux::unexpected(*invalid);
  }
  for (const auto& [key, value] : arguments) {
    const auto parameter = std::ranges::find(tool.schema.input, key, &Parameter::name);
    if (parameter == tool.schema.input.end()) {
      return libtmux::unexpected(ToolError{true, "unknown argument: " + key});
    }
    if (parameter->maximum_length.has_value()) {
      const auto length = utf8_code_points(value);
      if (!length.has_value()) {
        return libtmux::unexpected(ToolError{true, key + " must contain valid UTF-8"});
      }
      if (*length > *parameter->maximum_length) {
        return libtmux::unexpected(ToolError{
            true, key + " is longer than " +
                      std::to_string(*parameter->maximum_length) + " characters"});
      }
    }
    if (!parameter->allowed_values.empty() &&
        std::ranges::find(parameter->allowed_values, value) ==
            parameter->allowed_values.end()) {
      return libtmux::unexpected(
          ToolError{true, key + " must be one of the advertised values"});
    }
    if (parameter->type == ArgumentType::integer) {
      if (auto parsed = parse_integer(value, *parameter); !parsed.has_value()) {
        return libtmux::unexpected(ToolError{true, parsed.error()});
      }
    } else if (parameter->type == ArgumentType::boolean && value != "true" &&
               value != "false") {
      return libtmux::unexpected(ToolError{true, key + " must be a boolean"});
    }
  }
  for (const auto& [key, values] : arguments.string_arrays) {
    const auto parameter = std::ranges::find(tool.schema.input, key, &Parameter::name);
    if (parameter == tool.schema.input.end() ||
        parameter->type != ArgumentType::string_array) {
      return libtmux::unexpected(ToolError{true, "unknown argument: " + key});
    }
    if (parameter->minimum.has_value() &&
        values.size() < static_cast<std::size_t>(*parameter->minimum)) {
      return libtmux::unexpected(ToolError{true, key + " has too few items"});
    }
    if (parameter->maximum.has_value() &&
        values.size() > static_cast<std::size_t>(*parameter->maximum)) {
      return libtmux::unexpected(ToolError{true, key + " has too many items"});
    }
    for (const std::string& value : values) {
      const auto length = utf8_code_points(value);
      if (!length.has_value()) {
        return libtmux::unexpected(ToolError{true, key + " must contain valid UTF-8"});
      }
      if (parameter->maximum_length.has_value() &&
          *length > *parameter->maximum_length) {
        return libtmux::unexpected(ToolError{
            true, key + " contains an item longer than " +
                      std::to_string(*parameter->maximum_length) + " characters"});
      }
    }
  }
  for (const Parameter& parameter : tool.schema.input) {
    if (!parameter.required) {
      continue;
    }
    if (parameter.type == ArgumentType::read_calls) {
      if (arguments.read_calls.empty()) {
        return libtmux::unexpected(
            ToolError{true, "missing required argument: " + parameter.name});
      }
      continue;
    }
    if (parameter.type == ArgumentType::send_key_operations) {
      if (arguments.send_key_operations.empty()) {
        return libtmux::unexpected(
            ToolError{true, "missing required argument: " + parameter.name});
      }
      continue;
    }
    if (parameter.type == ArgumentType::string_array) {
      if (!arguments.string_arrays.contains(parameter.name) ||
          arguments.string_arrays.at(parameter.name).empty()) {
        return libtmux::unexpected(
            ToolError{true, "missing required argument: " + parameter.name});
      }
      continue;
    }
    const std::string* const value = argument(arguments, parameter.name);
    if (value == nullptr || value->empty()) {
      return libtmux::unexpected(
          ToolError{true, "missing required argument: " + parameter.name});
    }
  }
  if (context.cancelled()) {
    return libtmux::unexpected(ToolError{false, "request cancelled"});
  }
  CallContext nested = context;
  if (!tool.authority.nested_tools.empty()) {
    nested.call_tool = [this, &server, &nested,
                        &tool](std::string_view nested_name,
                               const Arguments& nested_arguments) {
      const ToolDefinition* const target = find_nested(tool, nested_name);
      return target == nullptr
                 ? ToolResult{libtmux::unexpected(
                       ToolError{true, "tool is not batch eligible: " +
                                           std::string{nested_name}})}
                 : call_definition(server, *target, nested_arguments, nested);
    };
    nested.can_call_read_tool = [this, &tool](std::string_view nested_name) {
      return find_nested(tool, nested_name) != nullptr;
    };
  }
  return tool.handler(server, arguments, nested);
}

libtmux::expected<ToolSelection, std::string> parse_tool_selection(
    std::optional<std::string_view> toolsets, std::optional<std::string_view> include,
    std::optional<std::string_view> exclude, bool teardown_enabled_by_default) {
  ToolSelection selection;
  if (!toolsets.has_value()) {
    selection.toolsets = {Toolset::inspect, Toolset::manage, Toolset::execute};
    if (teardown_enabled_by_default) {
      selection.toolsets.insert(Toolset::teardown);
    }
  } else {
    auto names = parse_names(toolsets, "LIBTMUX_TOOLSETS", true);
    if (!names.has_value()) {
      return libtmux::unexpected(names.error());
    }
    for (const std::string& name : *names) {
      if (name == "inspect") {
        selection.toolsets.insert(Toolset::inspect);
      } else if (name == "manage") {
        selection.toolsets.insert(Toolset::manage);
      } else if (name == "execute") {
        selection.toolsets.insert(Toolset::execute);
      } else if (name == "teardown") {
        selection.toolsets.insert(Toolset::teardown);
      } else {
        return libtmux::unexpected("unknown LIBTMUX_TOOLSETS name: " + name);
      }
    }
  }
  auto included = parse_names(include, "LIBTMUX_TOOLS", false);
  if (!included.has_value()) {
    return libtmux::unexpected(included.error());
  }
  selection.include.insert(included->begin(), included->end());
  auto excluded = parse_names(exclude, "LIBTMUX_EXCLUDE_TOOLS", false);
  if (!excluded.has_value()) {
    return libtmux::unexpected(excluded.error());
  }
  selection.exclude.insert(excluded->begin(), excluded->end());
  return selection;
}

libtmux::expected<ToolRegistry, std::string> configured_tools() {
  return configured_tools(true);
}

libtmux::expected<ToolRegistry, std::string>
configured_tools(bool teardown_enabled_by_default) {
  if (std::getenv("LIBTMUX_SAFETY") != nullptr) {
    return libtmux::unexpected(
        std::string{"LIBTMUX_SAFETY is retired; use LIBTMUX_TOOLSETS, "
                    "LIBTMUX_TOOLS, and LIBTMUX_EXCLUDE_TOOLS"});
  }
  auto selection = parse_tool_selection(
      environment("LIBTMUX_TOOLSETS"), environment("LIBTMUX_TOOLS"),
      environment("LIBTMUX_EXCLUDE_TOOLS"), teardown_enabled_by_default);
  if (!selection.has_value()) {
    return libtmux::unexpected(selection.error());
  }
  return default_tools(*selection);
}

} // namespace libtmux::mcp
