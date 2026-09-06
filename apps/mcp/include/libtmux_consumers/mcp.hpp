#pragma once

// A format-independent MCP tool surface over libtmux.

#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "libtmux/abi.hpp"
#include "libtmux/expected.hpp"

LIBTMUX_NAMESPACE_BEGIN
class Server;
LIBTMUX_NAMESPACE_END

namespace libtmux::mcp {

using ArgumentMap = std::map<std::string, std::string, std::less<>>;

struct FlatArguments : ArgumentMap {
  FlatArguments() = default;
  FlatArguments(
      std::initializer_list<std::pair<std::string_view, std::string_view>> values) {
    for (const auto& [name, value] : values) {
      emplace(name, value);
    }
  }
  explicit FlatArguments(ArgumentMap values) : ArgumentMap{std::move(values)} {}

  std::map<std::string, std::vector<std::string>, std::less<>> string_arrays;
};

struct ReadToolCall {
  std::string tool;
  FlatArguments arguments;
};

struct Arguments : FlatArguments {
  Arguments() = default;
  Arguments(
      std::initializer_list<std::pair<std::string_view, std::string_view>> values) {
    for (const auto& [name, value] : values) {
      emplace(name, value);
    }
  }
  explicit Arguments(FlatArguments values) : FlatArguments{std::move(values)} {}

  std::vector<ReadToolCall> read_calls;
  std::vector<FlatArguments> send_key_operations;
};

struct ToolError {
  bool caller_error{};
  std::string message;
};

struct StructuredValue {
  using Array = std::vector<StructuredValue>;
  using Object = std::map<std::string, StructuredValue, std::less<>>;
  using Storage =
      std::variant<std::nullptr_t, bool, std::int64_t, std::string, Array, Object>;

  StructuredValue() : value{nullptr} {}
  StructuredValue(bool item) : value{item} {}
  template <std::integral Integer>
    requires(!std::same_as<std::remove_cv_t<Integer>, bool>)
  StructuredValue(Integer item) : value{static_cast<std::int64_t>(item)} {}
  StructuredValue(std::string item) : value{std::move(item)} {}
  StructuredValue(std::string_view item) : value{std::string{item}} {}
  StructuredValue(const char* item) : value{std::string{item}} {}
  StructuredValue(Array item) : value{std::move(item)} {}
  StructuredValue(Object item) : value{std::move(item)} {}

  Storage value;
};

struct ToolOutput {
  StructuredValue::Object structured;
  std::optional<std::size_t> maximum_response_bytes{};
};

using ToolResult = libtmux::expected<ToolOutput, ToolError>;

struct CallContext {
  std::function<bool()> is_cancelled{};
  std::function<void(double, std::optional<double>, std::string)> progress{};
  std::function<ToolResult(std::string_view, const Arguments&)> call_tool{};
  std::function<bool(std::string_view)> can_call_read_tool{};

  [[nodiscard]] bool cancelled() const noexcept {
    return is_cancelled && is_cancelled();
  }

  void report(double completed, std::optional<double> total,
              std::string message) const {
    if (progress) {
      progress(completed, total, std::move(message));
    }
  }
};

using Handler =
    std::function<ToolResult(const Server&, const Arguments&, const CallContext&)>;

enum class ArgumentType : std::uint8_t {
  string,
  integer,
  boolean,
  string_array,
  read_calls,
  send_key_operations,
};

struct Parameter {
  std::string name;
  std::string description;
  ArgumentType type{ArgumentType::string};
  bool required{true};
  std::optional<long long> minimum{};
  std::optional<long long> maximum{};
  std::optional<std::size_t> maximum_length{};
  std::vector<std::string> allowed_values;
  bool allow_empty{};
};

enum class OutputShape : std::uint8_t {
  overview,
  sessions,
  windows,
  panes,
  pane_text,
  pane_id,
  pane_targets,
  session_id,
  window_id,
  wait,
  matches,
  object,
};

struct ToolAnnotations {
  bool read_only{};
  bool destructive{};
  bool idempotent{};
  bool open_world{};

  auto operator<=>(const ToolAnnotations&) const = default;
};

inline constexpr ToolAnnotations kConservativeAnnotations{false, true, false, true};

enum class Toolset : std::uint8_t { inspect, manage, execute, teardown };

enum class ProcessReach : std::uint8_t {
  none,
  configured_process,
  pane_input,
  pane_command,
};

enum class Effect : std::uint8_t { observe, change, delete_ };

enum class OutputClass : std::uint8_t {
  tmux_metadata,
  terminal_content,
  process_environment,
  configured_command,
};

enum class InputSink : std::uint8_t {
  none,
  tmux_lookup,
  tmux_state,
  pane_input,
  shell_command,
  process_argv,
  regex,
  tmux_format,
  nested_tool,
};

enum class NestedAuthority : std::uint8_t { none, controlled, unrestricted };

enum class InputControl : std::uint8_t {
  none,
  double_hash_once,
  validated_variable_name,
};

struct Sink {
  InputSink type{InputSink::none};
  NestedAuthority nested_authority{NestedAuthority::none};
  InputControl control{InputControl::none};

  auto operator<=>(const Sink&) const = default;
};

struct ToolAuthority {
  ProcessReach process_reach{ProcessReach::none};
  std::set<Effect> effects;
  std::set<OutputClass> output_classes;
  bool may_expose_secrets{};
  bool may_return_untrusted_content{};
  bool amplifies_future_input{};
  std::map<std::string, std::set<Sink>, std::less<>> input_sinks;
  std::set<std::string, std::less<>> nested_tools;
};

struct ToolSchema {
  std::vector<Parameter> input;
  OutputShape output{OutputShape::overview};
};

struct ToolDefinition {
  std::string name;
  std::string title;
  std::string description;
  Toolset toolset{Toolset::inspect};
  ToolAuthority authority;
  ToolAnnotations annotations;
  ToolSchema schema;
  Handler handler;

  [[nodiscard]] std::vector<std::string> required_names() const;
};

struct ToolSelection {
  std::set<Toolset> toolsets;
  std::set<std::string, std::less<>> include;
  std::set<std::string, std::less<>> exclude;

  [[nodiscard]] static ToolSelection all();
};

class ToolRegistry {
public:
  [[nodiscard]] static libtmux::expected<ToolRegistry, std::string>
  create(std::vector<ToolDefinition> definitions, const ToolSelection& selection);

  [[nodiscard]] std::span<const ToolDefinition> tools() const noexcept;
  [[nodiscard]] const ToolSelection& selection() const noexcept;
  [[nodiscard]] const ToolDefinition* find(std::string_view name) const noexcept;
  [[nodiscard]] const ToolDefinition* find_nested(const ToolDefinition& caller,
                                                  std::string_view name) const noexcept;
  [[nodiscard]] ToolResult call(const Server& server, std::string_view name,
                                const Arguments& arguments,
                                const CallContext& context = {}) const;

private:
  explicit ToolRegistry(std::vector<ToolDefinition> definitions,
                        std::size_t visible_count, ToolSelection selection)
      : definitions_{std::move(definitions)}, visible_count_{visible_count},
        selection_{std::move(selection)} {}

  [[nodiscard]] const ToolDefinition* find_any(std::string_view name) const noexcept;
  [[nodiscard]] ToolResult call_definition(const Server& server,
                                           const ToolDefinition& tool,
                                           const Arguments& arguments,
                                           const CallContext& context) const;

  std::vector<ToolDefinition> definitions_;
  std::size_t visible_count_{};
  ToolSelection selection_;
};

[[nodiscard]] libtmux::expected<ToolSelection, std::string> parse_tool_selection(
    std::optional<std::string_view> toolsets, std::optional<std::string_view> include,
    std::optional<std::string_view> exclude, bool teardown_enabled_by_default = true);
[[nodiscard]] libtmux::expected<ToolRegistry, std::string> default_tools();
[[nodiscard]] libtmux::expected<ToolRegistry, std::string>
default_tools(const ToolSelection& selection);
[[nodiscard]] libtmux::expected<ToolRegistry, std::string> configured_tools();
[[nodiscard]] libtmux::expected<ToolRegistry, std::string>
configured_tools(bool teardown_enabled_by_default);

} // namespace libtmux::mcp
