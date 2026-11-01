#include "cli.hpp"
#include "environment_value.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

namespace libtmux::mcp::server {
namespace {

[[nodiscard]] libtmux::expected<void, std::string>
set_socket(CliOptions& options, std::string_view selector) {
  if (selector == "inherit") {
    options.selector = Selector::inherit;
    options.value.clear();
    return {};
  }
  if (selector.starts_with("name:")) {
    const std::string_view value = selector.substr(5U);
    if (value.empty()) {
      return libtmux::unexpected(std::string{"socket name is empty"});
    }
    options.selector = Selector::name;
    options.value = value;
    return {};
  }
  if (selector.starts_with("path:")) {
    const std::string_view value = selector.substr(5U);
    if (value.empty() || !std::filesystem::path{value}.is_absolute()) {
      return libtmux::unexpected(std::string{"socket path must be absolute"});
    }
    options.selector = Selector::path;
    options.value = value;
    return {};
  }
  if (selector.empty()) {
    return libtmux::unexpected(std::string{"socket selector is empty"});
  }
  // The target comment's configuration table retains bare names. Accepting
  // one is unambiguous because paths require the `path:` prefix.
  options.selector = Selector::name;
  options.value = selector;
  return {};
}

[[nodiscard]] libtmux::expected<void, std::string>
set_configuration(CliOptions& options, std::string_view configuration) {
  const std::filesystem::path path{configuration};
  if (configuration.empty() || !path.is_absolute()) {
    return libtmux::unexpected(std::string{"tmux configuration path must be absolute"});
  }
  options.configuration_path = path.string();
  return {};
}

[[nodiscard]] std::string inherited_socket_path() {
  const auto inherited = libtmux::mcp::detail::environment_value("TMUX");
  if (!inherited.has_value()) {
    return {};
  }
  std::string_view value{*inherited};
  const auto last = value.find_last_of(',');
  const auto previous = last == std::string_view::npos
                            ? std::string_view::npos
                            : value.find_last_of(',', last - 1U);
  return std::string{previous == std::string_view::npos ? value
                                                        : value.substr(0U, previous)};
}

// The configuration a server this process starts reads: the one named, and
// otherwise the user's own. A server that is already running keeps its own.
[[nodiscard]] std::optional<std::filesystem::path>
startup_configuration(const CliOptions& options) {
  if (options.selector == Selector::inherit || options.configuration_path.empty()) {
    return std::nullopt;
  }
  return std::filesystem::path{options.configuration_path};
}

[[nodiscard]] bool means_absent(const libtmux::CommandFailure& failure) noexcept {
  // A tmux that cannot be launched at all is a daemon that is certainly not
  // running, so it reads as absent rather than as a provenance the server
  // failed to establish. POSIX reaches that conclusion through `missing`,
  // because a probe there runs a command against a socket; Windows probes by
  // listing psmux sessions, which reports `spawn` when the executable itself
  // is not there. Without this the server exits before answering initialize
  // on a Windows host with no psmux, where every other platform starts and
  // reports the daemon absent.
  return failure.kind == libtmux::FailureKind::refused ||
         failure.kind == libtmux::FailureKind::missing ||
         failure.kind == libtmux::FailureKind::spawn;
}

[[nodiscard]] libtmux::expected<void, libtmux::CommandFailure>
probe_daemon(const libtmux::Server& server) {
#if defined(_WIN32)
  return server.check_alive(std::chrono::milliseconds{250});
#else
  auto probe = server.run({"show-options", "-sqv", "exit-empty"},
                          std::chrono::milliseconds{250});
  if (!probe.has_value()) {
    return libtmux::unexpected(probe.error());
  }
  return {};
#endif
}

[[nodiscard]] std::string selector_text(const CliOptions& options) {
  if (options.selector == Selector::name) {
    return "name:" + options.value;
  }
  if (options.selector == Selector::path) {
    return "path:" + options.value;
  }
  if (options.selector == Selector::none) {
    return "none";
  }
  return "inherit";
}

} // namespace

libtmux::expected<CliOptions, std::string> parse_cli(int argc, char** argv) {
  CliOptions options;
  if (const auto configuration =
          libtmux::mcp::detail::environment_value("LIBTMUX_TMUX_CONFIG");
      configuration.has_value()) {
    if (auto selected = set_configuration(options, *configuration);
        !selected.has_value()) {
      return libtmux::unexpected(selected.error());
    }
  }

  bool socket_flag = false;
  bool configuration_flag = false;
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
    if (argument == "--socket" || argument == "--socket-path" ||
        argument == "--socket-name") {
      if (index + 1 >= argc || socket_flag) {
        return libtmux::unexpected(std::string{"choose exactly one tmux selector"});
      }
      socket_flag = true;
      std::string selector;
      if (argument == "--socket-path") {
        selector = "path:";
      } else if (argument == "--socket-name") {
        selector = "name:";
      }
      selector += argv[++index];
      if (auto selected = set_socket(options, selector); !selected.has_value()) {
        return libtmux::unexpected(selected.error());
      }
      options.selection_provenance = "explicit";
      continue;
    }
    if (argument == "--tmux-config") {
      if (index + 1 >= argc || configuration_flag) {
        return libtmux::unexpected(
            std::string{"choose exactly one tmux configuration"});
      }
      configuration_flag = true;
      if (auto selected = set_configuration(options, argv[++index]);
          !selected.has_value()) {
        return libtmux::unexpected(selected.error());
      }
      continue;
    }
    if (argument.starts_with('-') || socket_flag) {
      return libtmux::unexpected("unknown or duplicate argument: " +
                                 std::string{argument});
    }
    socket_flag = true;
    if (auto selected = set_socket(options, "path:" + std::string{argument});
        !selected.has_value()) {
      return libtmux::unexpected(selected.error());
    }
    options.selection_provenance = "explicit";
  }
  if (options.selector == Selector::inherit && inherited_socket_path().empty()) {
    // Asked for the `TMUX` socket, and there is none. Falling back to the
    // default would let the socket variables choose beside an option.
    return libtmux::unexpected(
        std::string{"--socket inherit needs TMUX to name a socket"});
  }
  if (socket_flag) {
    return options;
  }

  // No option named a socket, so the variables decide, and with neither set
  // nothing does and tmux picks the server. A relative path reaches what
  // `tmux -S` reaches from this working directory; it is made absolute below.
  const auto chosen = libtmux::socket_from_environment();
  if (chosen.source == libtmux::SocketSource::none) {
    return options;
  }
  if (const auto usable = chosen.arguments(); !usable.has_value()) {
    return libtmux::unexpected(std::string{libtmux::to_string(chosen.source)} + ": " +
                               std::string{libtmux::to_string(usable.error())});
  }
  const bool by_path = chosen.source == libtmux::SocketSource::path_variable;
  options.selector = by_path ? Selector::path : Selector::name;
  options.value = chosen.value;
  if (by_path) {
    // Reported, and handed to the attach command, as the absolute path it is,
    // unless that no longer fits a socket address: tmux takes the relative
    // path as given, and a working directory this deep must not refuse it.
    std::error_code failed;
    if (auto absolute = std::filesystem::absolute(options.value, failed);
        !failed && absolute.string().size() <= libtmux::kSocketPathLimit) {
      options.value = absolute.string();
    }
  }
  options.selection_provenance = by_path ? "env-socket-path" : "env-socket";
  return options;
}

libtmux::expected<OpenedServer, std::string> open_server(const CliOptions& options) {
  const auto configuration = startup_configuration(options);
  auto opened = [&]() -> libtmux::expected<libtmux::Server, libtmux::CommandFailure> {
    if (options.selector == Selector::path) {
      return libtmux::Server::startable_at_socket_path(options.value, configuration);
    }
    if (options.selector == Selector::name) {
      return libtmux::Server::startable_at_socket_name(options.value, configuration);
    }
    if (options.selector == Selector::inherit) {
      return libtmux::Server::startable_at_socket_path(inherited_socket_path(),
                                                       std::nullopt);
    }
    // Nothing chose a socket: the core leaves the server to tmux.
    return libtmux::Server::startable_at_default(configuration);
  }();
  if (!opened.has_value()) {
    return libtmux::unexpected(opened.error().diagnostic);
  }

  const auto liveness = probe_daemon(*opened);
  const bool existing = liveness.has_value();
  if (!existing && !means_absent(liveness.error())) {
    return libtmux::unexpected("could not establish tmux daemon provenance: " +
                               liveness.error().diagnostic);
  }

  // A server that is running chose its own configuration. One this process
  // may start reads the file named, when one was.
  const bool user_configured = !existing && options.selector != Selector::inherit &&
                               !options.configuration_path.empty();
  return OpenedServer{.server = *std::move(opened),
                      .socket_selector = selector_text(options),
                      .socket_provenance = options.selection_provenance,
                      .configuration_provenance =
                          user_configured ? "user-configured" : "unknown",
                      .server_pre_existing = existing};
}

void print_usage(std::ostream& output) {
  output << "Usage: libtmux-mcp-server [--socket SELECTOR] "
            "[--tmux-config /ABSOLUTE/PATH]\n"
            "       SELECTOR is name:NAME, path:/ABSOLUTE/PATH, or inherit\n"
            "       With no selector, LIBTMUX_SOCKET_PATH then LIBTMUX_SOCKET names\n"
            "       the socket, and with neither tmux picks the server\n";
}

} // namespace libtmux::mcp::server
