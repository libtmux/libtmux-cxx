#include <cstdio>
#include <iostream>
#include <utility>

#include "cli.hpp"
#include "libtmux/version.hpp"
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
  auto server = open_server(*options);
  if (!server.has_value()) {
    std::fprintf(stderr, "libtmux-mcp: %s\n", server.error().c_str());
    return 1;
  }
  return serve_stdio(*std::move(server));
}
