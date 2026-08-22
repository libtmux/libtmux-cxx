#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "libtmux/server.hpp"
#include "libtmux_consumers/mcp.hpp"

namespace libtmux::mcp::detail {

inline constexpr std::size_t kTargetCharacters = 512U;
inline constexpr std::size_t kSearchCharacters = 4096U;

[[nodiscard]] const std::string* argument(const Arguments& arguments,
                                          std::string_view name);
[[nodiscard]] ToolOutput output(StructuredValue::Object structured);
[[nodiscard]] ToolError tmux_error(const CommandFailure& error);
[[nodiscard]] StructuredValue session_value(const Session& session);
[[nodiscard]] StructuredValue window_value(const Window& window);
[[nodiscard]] StructuredValue pane_value(const Pane& pane);

} // namespace libtmux::mcp::detail
