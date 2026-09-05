#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "libtmux_consumers/mcp.hpp"
#include "protocol_types.hpp"

namespace libtmux::mcp::server {

struct CapabilityDisclosure {
  std::string selector;
  std::string selection_provenance;
  std::string server_state;
  std::string configuration_provenance;
  std::string resolved_socket_path;
  std::string attach_command;
};

[[nodiscard]] json modern_protocol_versions();
[[nodiscard]] json initialize_result(std::string_view version);
[[nodiscard]] json discover_result();
[[nodiscard]] json ping_result();
[[nodiscard]] json tools_result(const ToolRegistry& tools, ProtocolEra era);
[[nodiscard]] json resources_result(ProtocolEra era);
[[nodiscard]] json
capabilities_resource_result(const ToolRegistry& tools, ProtocolEra era,
                             const CapabilityDisclosure& disclosure = {});
[[nodiscard]] json tool_success(const ToolOutput& answer, ProtocolEra era);
[[nodiscard]] json tool_success(const ToolOutput& answer, ProtocolEra era,
                                std::size_t maximum_result_bytes);
[[nodiscard]] json tool_failure(std::string message, ProtocolEra era);

} // namespace libtmux::mcp::server
