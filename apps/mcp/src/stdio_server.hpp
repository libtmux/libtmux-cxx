#pragma once

#include "libtmux/server.hpp"
#include "libtmux_consumers/mcp.hpp"
#include "schema.hpp"

namespace libtmux::mcp::server {

int serve_stdio(libtmux::Server server, ToolRegistry tools,
                CapabilityDisclosure disclosure = {});

} // namespace libtmux::mcp::server
