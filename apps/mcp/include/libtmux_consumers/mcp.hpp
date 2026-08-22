#pragma once

// A format-independent MCP tool surface over libtmux.

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
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

using Arguments = std::map<std::string, std::string, std::less<>>;

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
};

using ToolResult = libtmux::expected<ToolOutput, ToolError>;

struct CallContext {
  std::function<bool()> is_cancelled{};
  std::function<void(double, std::optional<double>, std::string)> progress{};

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

enum class ArgumentType : std::uint8_t { string, integer };

struct Parameter {
  std::string name;
  std::string description;
  ArgumentType type{ArgumentType::string};
  bool required{true};
  std::optional<long long> minimum{};
  std::optional<long long> maximum{};
  std::optional<std::size_t> maximum_length{};
};

enum class OutputShape : std::uint8_t {
  overview,
  sessions,
  windows,
  panes,
  pane_text,
  pane_id,
  session_id,
  window_id,
  wait,
  matches,
};

struct ToolAnnotations {
  bool read_only{};
  bool destructive{};
  bool idempotent{};
  bool open_world{};
};

struct Tool {
  std::string name;
  std::string title;
  std::string description;
  std::vector<Parameter> parameters;
  OutputShape output;
  ToolAnnotations annotations;
  Handler handle;

  [[nodiscard]] std::vector<std::string> required_names() const;
};

class ToolSet {
public:
  void add(Tool tool);

  [[nodiscard]] const std::vector<Tool>& tools() const noexcept;
  [[nodiscard]] const Tool* find(std::string_view name) const noexcept;
  [[nodiscard]] ToolResult call(const Server& server, std::string_view name,
                                const Arguments& arguments,
                                const CallContext& context = {}) const;

private:
  std::vector<Tool> tools_;
};

[[nodiscard]] ToolSet default_tools();

} // namespace libtmux::mcp
