#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>

#include "libtmux/expected.hpp"
#include "libtmux/server.hpp"

namespace libtmux::mcp::server {

enum class Selector : std::uint8_t { automatic, path, name };

struct CliOptions {
  Selector selector{Selector::automatic};
  std::string value;
  bool help{};
  bool version{};
};

[[nodiscard]] libtmux::expected<CliOptions, std::string> parse_cli(int argc,
                                                                   char** argv);
[[nodiscard]] libtmux::expected<libtmux::Server, std::string>
open_server(const CliOptions& options);
void print_usage(std::ostream& output);

} // namespace libtmux::mcp::server
