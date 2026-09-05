#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>

#include "libtmux/expected.hpp"
#include "libtmux/server.hpp"

namespace libtmux::mcp::server {

enum class Selector : std::uint8_t { path, name, inherit };
enum class Configuration : std::uint8_t { minimal, path };

struct CliOptions {
  Selector selector{Selector::name};
  std::string value{"libtmux-mcp"};
  Configuration configuration{Configuration::minimal};
  std::string configuration_path;
  std::string packaged_configuration_path;
  bool explicit_selector{};
  bool help{};
  bool version{};
};

struct OpenedServer {
  libtmux::Server server;
  std::string socket_selector;
  std::string socket_provenance;
  std::string configuration_provenance;
  bool server_pre_existing{};
  bool teardown_enabled_by_default{};
};

[[nodiscard]] libtmux::expected<CliOptions, std::string> parse_cli(int argc,
                                                                   char** argv);
[[nodiscard]] libtmux::expected<OpenedServer, std::string>
open_server(const CliOptions& options);
[[nodiscard]] std::filesystem::path
minimal_configuration_path(std::filesystem::path executable = {});
void print_usage(std::ostream& output);

} // namespace libtmux::mcp::server
