#include <cstdio>
#include <iostream>
#include <string>
#include <utility>

#include "cli.hpp"
#include "libtmux/version.hpp"
#include "libtmux_consumers/mcp.hpp"
#include "stdio_server.hpp"

int main(int argc, char** argv) {
  using namespace libtmux::mcp::server;

  const auto options = parse_cli(argc, argv);
  if (!options.has_value()) {
    std::fprintf(stderr, "libtmux-mcp: %s\n", options.error().c_str());
    return 2;
  }
  if (options->help) {
    print_usage(std::cout);
    return 0;
  }
  if (options->version) {
    std::cout << libtmux::library_version() << '\n';
    return 0;
  }
  // Validate every startup-frozen policy name before resolving or opening a
  // socket. Teardown is out of the default toolsets: nothing here creates a
  // server of its own for it to end, and `LIBTMUX_TOOLSETS` grants it.
  auto tools = libtmux::mcp::configured_tools(false);
  if (!tools.has_value()) {
    std::fprintf(stderr, "libtmux-mcp: %s\n", tools.error().c_str());
    return 2;
  }
  auto opened = open_server(*options);
  if (!opened.has_value()) {
    std::fprintf(stderr, "libtmux-mcp: %s\n", opened.error().c_str());
    return 1;
  }
  const std::string resolved_socket{opened->server.socket_path()};
  const std::string state = opened->server_pre_existing ? "existing" : "absent";
  CapabilityDisclosure disclosure =
      capability_disclosure(opened->server.capabilities().implementation,
                            opened->socket_selector, opened->socket_provenance, state,
                            opened->configuration_provenance, resolved_socket);
  std::fprintf(stderr,
               "libtmux-mcp: socket=%s resolved_socket_path=%s server_state=%s "
               "configuration_provenance=%s tool_count=%zu\n",
               opened->socket_selector.c_str(), resolved_socket.c_str(), state.c_str(),
               opened->configuration_provenance.c_str(), tools->tools().size());
  return serve_stdio(std::move(opened->server), *std::move(tools),
                     std::move(disclosure));
}
