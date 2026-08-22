#include "cli.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cstdlib>
#endif

#include <ostream>
#include <string>
#include <string_view>
#include <utility>

namespace libtmux::mcp::server {
namespace {

[[nodiscard]] bool has_inherited_tmux() {
#if defined(_WIN32)
  return GetEnvironmentVariableW(L"TMUX", nullptr, 0) != 0;
#else
  return std::getenv("TMUX") != nullptr;
#endif
}

} // namespace

libtmux::expected<CliOptions, std::string> parse_cli(int argc, char** argv) {
  CliOptions options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (argument == "--help" || argument == "-h") {
      options.help = true;
      continue;
    }
    if (argument == "--version") {
      options.version = true;
      continue;
    }
    if (argument == "--socket-path" || argument == "--socket-name") {
      if (index + 1 >= argc || options.selector != Selector::automatic ||
          !options.value.empty()) {
        return libtmux::unexpected(std::string{"choose exactly one tmux selector"});
      }
      options.selector = argument == "--socket-path" ? Selector::path : Selector::name;
      options.value = argv[++index];
      continue;
    }
    if (argument.starts_with('-') || options.selector != Selector::automatic ||
        !options.value.empty()) {
      return libtmux::unexpected("unknown or duplicate argument: " +
                                 std::string{argument});
    }
    options.selector = Selector::path;
    options.value = argument;
  }
  return options;
}

libtmux::expected<libtmux::Server, std::string> open_server(const CliOptions& options) {
  if (options.selector == Selector::automatic && !has_inherited_tmux()) {
    return libtmux::unexpected(
        std::string{"no tmux route; pass --socket-path or --socket-name"});
  }
  const auto server = [&] {
    if (options.selector == Selector::path) {
      return libtmux::Server::at_socket_path(options.value);
    }
    if (options.selector == Selector::name) {
      return libtmux::Server::at_socket_name(options.value);
    }
    return libtmux::Server::from_env();
  }();
  if (!server.has_value()) {
    return libtmux::unexpected(server.error().diagnostic);
  }
  return *std::move(server);
}

void print_usage(std::ostream& output) {
  output << "Usage: libtmux-mcp-server "
            "[--socket-path PATH | --socket-name NAME]\n"
            "       libtmux-mcp-server [PATH]\n"
            "       libtmux-mcp-server  (with an inherited TMUX route)\n";
}

} // namespace libtmux::mcp::server
