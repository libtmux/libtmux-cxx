#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>

#include "libtmux/expected.hpp"
#include "libtmux/server.hpp"

namespace libtmux::mcp::server {

enum class Selector : std::uint8_t { none, path, name, inherit };

struct CliOptions {
  Selector selector{Selector::none};
  std::string value;
  std::string configuration_path;
  // What chose the socket: `explicit` for an option or a flag, `env-socket-path`
  // or `env-socket` for a variable, and `tmux-choice` when nothing did and tmux
  // picks the server.
  std::string selection_provenance{"tmux-choice"};
  bool help{};
  bool version{};
};

struct OpenedServer {
  libtmux::Server server;
  std::string socket_selector;
  std::string socket_provenance;
  std::string configuration_provenance;
  bool server_pre_existing{};
};

[[nodiscard]] libtmux::expected<CliOptions, std::string> parse_cli(int argc,
                                                                   char** argv);
[[nodiscard]] libtmux::expected<OpenedServer, std::string>
open_server(const CliOptions& options);
void print_usage(std::ostream& output);

} // namespace libtmux::mcp::server
