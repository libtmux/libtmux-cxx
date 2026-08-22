#pragma once

#include "libtmux_consumers/mcp.hpp"

namespace libtmux::mcp::detail {

[[nodiscard]] ToolResult wait_for_text(const Server& server, const Arguments& arguments,
                                       const CallContext& context);

} // namespace libtmux::mcp::detail
