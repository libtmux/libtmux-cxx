#include "libtmux_consumers/mcp.hpp"

#include <algorithm>
#include <charconv>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include "libtmux/server.hpp"

namespace libtmux::mcp {
namespace {

[[nodiscard]] const std::string* argument(const Arguments& arguments,
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

} // namespace

std::vector<std::string> Tool::required_names() const {
  std::vector<std::string> names;
  for (const Parameter& parameter : parameters) {
    if (parameter.required) {
      names.push_back(parameter.name);
    }
  }
  return names;
}

void ToolSet::add(Tool tool) { tools_.push_back(std::move(tool)); }

const std::vector<Tool>& ToolSet::tools() const noexcept { return tools_; }

const Tool* ToolSet::find(std::string_view name) const noexcept {
  const auto found = std::ranges::find(tools_, name, &Tool::name);
  return found == tools_.end() ? nullptr : &*found;
}

ToolResult ToolSet::call(const Server& server, std::string_view name,
                         const Arguments& arguments, const CallContext& context) const {
  const Tool* const tool = find(name);
  if (tool == nullptr) {
    return libtmux::unexpected(ToolError{true, "unknown tool: " + std::string{name}});
  }
  for (const auto& [key, value] : arguments) {
    const auto parameter = std::ranges::find(tool->parameters, key, &Parameter::name);
    if (parameter == tool->parameters.end()) {
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
    if (parameter->type == ArgumentType::integer) {
      if (auto parsed = parse_integer(value, *parameter); !parsed.has_value()) {
        return libtmux::unexpected(ToolError{true, parsed.error()});
      }
    }
  }
  for (const Parameter& parameter : tool->parameters) {
    if (!parameter.required) {
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
  return tool->handle(server, arguments, context);
}

} // namespace libtmux::mcp
