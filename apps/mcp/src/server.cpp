#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#include "cli.hpp"
#include "libtmux/version.hpp"
#include "libtmux_consumers/mcp.hpp"
#include "stdio_server.hpp"

namespace {

[[nodiscard]] std::string shell_quote(std::string_view value) {
  std::string quoted{"'"};
  for (const char character : value) {
    if (character == '\'') {
      quoted += "'\"'\"'";
    } else {
      quoted += character;
    }
  }
  quoted += '\'';
  return quoted;
}

} // namespace

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
  // socket. The provenance-aware default is applied again after the socket is
  // classified; explicit selections have identical meaning in both passes.
  auto validated_tools = libtmux::mcp::configured_tools(false);
  if (!validated_tools.has_value()) {
    std::fprintf(stderr, "libtmux-mcp: %s\n", validated_tools.error().c_str());
    return 2;
  }
  auto opened = open_server(*options);
  if (!opened.has_value()) {
    std::fprintf(stderr, "libtmux-mcp: %s\n", opened.error().c_str());
    return 1;
  }
  auto tools = libtmux::mcp::configured_tools(opened->teardown_enabled_by_default);
  if (!tools.has_value()) {
    std::fprintf(stderr, "libtmux-mcp: %s\n", tools.error().c_str());
    return 2;
  }
  const std::string resolved_socket{opened->server.socket_path()};
  const std::string state = opened->server_pre_existing           ? "existing"
                            : opened->teardown_enabled_by_default ? "created"
                                                                  : "absent";
  CapabilityDisclosure disclosure{
      .selector = opened->socket_selector,
      .selection_provenance = opened->socket_provenance,
      .server_state = state,
      .configuration_provenance = opened->configuration_provenance,
      .resolved_socket_path = resolved_socket,
      .attach_command = "tmux -N -S " + shell_quote(resolved_socket) + " attach"};
  std::fprintf(stderr,
               "libtmux-mcp: socket=%s resolved_socket_path=%s server_state=%s "
               "configuration_provenance=%s tool_count=%zu\n",
               opened->socket_selector.c_str(), resolved_socket.c_str(), state.c_str(),
               opened->configuration_provenance.c_str(), tools->tools().size());
  return serve_stdio(std::move(opened->server), *std::move(tools),
                     std::move(disclosure));
}
