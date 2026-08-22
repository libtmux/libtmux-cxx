#pragma once

#include <string>
#include <string_view>

#include "libtmux_consumers/mcp.hpp"
#include "protocol_types.hpp"

namespace libtmux::mcp::server {

[[nodiscard]] json modern_protocol_versions();
[[nodiscard]] json initialize_result(std::string_view version);
[[nodiscard]] json discover_result();
[[nodiscard]] json ping_result();
[[nodiscard]] json tools_result(const ToolSet& tools, ProtocolEra era);
[[nodiscard]] json tool_success(const ToolOutput& answer, ProtocolEra era);
[[nodiscard]] json tool_failure(std::string message, ProtocolEra era);

} // namespace libtmux::mcp::server
