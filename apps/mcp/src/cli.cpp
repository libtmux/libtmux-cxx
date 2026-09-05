#include "cli.hpp"

#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <optional>
#include <ostream>
#include <random>
#include <sstream>
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
  options.configuration = Configuration::path;
  options.configuration_path = path.string();
  return {};
}

[[nodiscard]] bool inherited_tmux() {
  const char* const value = std::getenv("TMUX");
  return value != nullptr && *value != '\0';
}

[[nodiscard]] std::string inherited_socket_path() {
  const char* const inherited = std::getenv("TMUX");
  if (inherited == nullptr) {
    return {};
  }
  std::string_view value{inherited};
  const auto last = value.find_last_of(',');
  const auto previous = last == std::string_view::npos
                            ? std::string_view::npos
                            : value.find_last_of(',', last - 1U);
  return std::string{previous == std::string_view::npos ? value
                                                        : value.substr(0U, previous)};
}

[[nodiscard]] std::optional<std::filesystem::path>
startup_configuration(const CliOptions& options) {
  if (options.selector == Selector::inherit) {
    return std::nullopt;
  }
  if (options.configuration == Configuration::path) {
    return std::filesystem::path{options.configuration_path};
  }
  if (!options.packaged_configuration_path.empty()) {
    return std::filesystem::path{options.packaged_configuration_path};
  }
  return minimal_configuration_path();
}

constexpr std::string_view provenance_environment{"LIBTMUX_MCP_OWNER"};
constexpr std::string_view provenance_option{"@libtmux_mcp_owner"};

class ScopedEnvironment {
public:
  explicit ScopedEnvironment(std::string value) : value_(std::move(value)) {
    if (const char* const previous = std::getenv(provenance_environment.data());
        previous != nullptr) {
      previous_ = previous;
    }
#if defined(_WIN32)
    active_ = _putenv_s(provenance_environment.data(), value_.c_str()) == 0;
#else
    active_ = ::setenv(provenance_environment.data(), value_.c_str(), 1) == 0;
#endif
  }
  ScopedEnvironment(const ScopedEnvironment&) = delete;
  ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;
  ~ScopedEnvironment() {
    if (!active_) {
      return;
    }
#if defined(_WIN32)
    static_cast<void>(
        _putenv_s(provenance_environment.data(), previous_.value_or("").c_str()));
#else
    if (previous_.has_value()) {
      static_cast<void>(::setenv(provenance_environment.data(), previous_->c_str(), 1));
    } else {
      static_cast<void>(::unsetenv(provenance_environment.data()));
    }
#endif
  }

  [[nodiscard]] bool active() const noexcept { return active_; }

private:
  std::string value_;
  std::optional<std::string> previous_;
  bool active_{};
};

[[nodiscard]] libtmux::expected<std::string, std::string> random_nonce() {
  try {
    std::random_device random;
    std::ostringstream text;
    text << std::hex << std::setfill('0');
    for (unsigned word = 0U; word < 4U; ++word) {
      text << std::setw(8) << random();
    }
    return text.str();
  } catch (const std::exception& error) {
    return libtmux::unexpected("could not generate startup provenance: " +
                               std::string{error.what()});
  }
}

[[nodiscard]] bool means_absent(const libtmux::CommandFailure& failure) noexcept {
  return failure.kind == libtmux::FailureKind::refused ||
         failure.kind == libtmux::FailureKind::missing;
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
  return "inherit";
}

} // namespace

std::filesystem::path minimal_configuration_path(std::filesystem::path executable) {
  if (!executable.empty()) {
    std::error_code error;
    if (!executable.has_parent_path()) {
      if (const char* const path = std::getenv("PATH"); path != nullptr) {
        std::string_view remaining{path};
        while (!remaining.empty()) {
          const auto separator = remaining.find(
#if defined(_WIN32)
              ';'
#else
              ':'
#endif
          );
          const std::filesystem::path directory{remaining.substr(0U, separator)};
          const auto candidate = directory / executable;
          if (std::filesystem::exists(candidate, error)) {
            executable = candidate;
            break;
          }
          if (separator == std::string_view::npos) {
            break;
          }
          remaining.remove_prefix(separator + 1U);
        }
      }
    }
    if (!executable.has_parent_path()) {
      executable = std::filesystem::absolute(executable, error);
    }
    const auto installed = executable.parent_path() / "libtmux-mcp-minimal.conf";
    if (std::filesystem::is_regular_file(installed, error)) {
      return installed;
    }
  }
#if defined(LIBTMUX_MCP_MINIMAL_CONFIG_SOURCE)
  return std::filesystem::path{LIBTMUX_MCP_MINIMAL_CONFIG_SOURCE};
#else
  return std::filesystem::path{"minimal.conf"};
#endif
}

libtmux::expected<CliOptions, std::string> parse_cli(int argc, char** argv) {
  CliOptions options;
  if (argc > 0 && argv[0] != nullptr) {
    options.packaged_configuration_path =
        minimal_configuration_path(std::filesystem::path{argv[0]}).string();
  }
  const char* const socket_name = std::getenv("LIBTMUX_SOCKET");
  const char* const socket_path = std::getenv("LIBTMUX_SOCKET_PATH");
  if (socket_name != nullptr && socket_path != nullptr) {
    return libtmux::unexpected(
        std::string{"LIBTMUX_SOCKET and LIBTMUX_SOCKET_PATH are mutually exclusive"});
  }
  if (socket_name != nullptr) {
    if (auto selected = set_socket(options, "name:" + std::string{socket_name});
        !selected.has_value()) {
      return libtmux::unexpected(selected.error());
    }
    options.explicit_selector = true;
  }
  if (socket_path != nullptr) {
    if (auto selected = set_socket(options, "path:" + std::string{socket_path});
        !selected.has_value()) {
      return libtmux::unexpected(selected.error());
    }
    options.explicit_selector = true;
  }
  if (const char* const configuration = std::getenv("LIBTMUX_TMUX_CONFIG");
      configuration != nullptr) {
    if (auto selected = set_configuration(options, configuration);
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
      options.explicit_selector = true;
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
    options.explicit_selector = true;
  }
  return options;
}

libtmux::expected<OpenedServer, std::string> open_server(const CliOptions& options) {
  const auto configuration = startup_configuration(options);
  const bool default_minimal = !options.explicit_selector &&
                               options.selector != Selector::inherit &&
                               options.configuration == Configuration::minimal;
  auto opened = [&]() -> libtmux::expected<libtmux::Server, libtmux::CommandFailure> {
    if (options.selector == Selector::path) {
      return libtmux::Server::startable_at_socket_path(options.value, configuration);
    }
    if (options.selector == Selector::name) {
      return libtmux::Server::startable_at_socket_name(options.value, configuration);
    }
    if (inherited_tmux()) {
      return libtmux::Server::startable_at_socket_path(inherited_socket_path(),
                                                       std::nullopt);
    }
    return libtmux::Server::startable_at_default(std::nullopt);
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

  bool newly_minimal = false;
  if (!existing && default_minimal) {
    auto nonce = random_nonce();
    if (!nonce.has_value()) {
      return libtmux::unexpected(nonce.error());
    }
    static std::mutex startup_environment_mutex;
    std::lock_guard guard{startup_environment_mutex};
    ScopedEnvironment owner{*nonce};
    if (!owner.active()) {
      return libtmux::unexpected(
          std::string{"could not set the startup provenance environment"});
    }
    const auto started = opened->run({"start-server"});
    if (!started.has_value()) {
      return libtmux::unexpected("could not establish the dedicated tmux daemon: " +
                                 started.error().diagnostic);
    }
    const auto marker =
        opened->run({"show-options", "-gqv", std::string{provenance_option}});
    if (!marker.has_value() || *marker != *nonce + "\n") {
      return libtmux::unexpected(
          std::string{"another tmux daemon won startup; provenance is unknown"});
    }
    newly_minimal = true;
  }

  std::string provenance{"unknown"};
  if (newly_minimal) {
    provenance = "minimal";
  } else if (!existing && options.selector != Selector::inherit) {
    if (options.configuration == Configuration::path) {
      provenance = "user-configured";
    }
  }
  return OpenedServer{.server = *std::move(opened),
                      .socket_selector = selector_text(options),
                      .socket_provenance = options.explicit_selector
                                               ? "operator-current"
                                               : "default-dedicated",
                      .configuration_provenance = std::move(provenance),
                      .server_pre_existing = existing,
                      .teardown_enabled_by_default = newly_minimal,
                      .owns_daemon = newly_minimal};
}

void print_usage(std::ostream& output) {
  output << "Usage: libtmux-mcp-server [--socket SELECTOR] "
            "[--tmux-config /ABSOLUTE/PATH]\n"
            "       SELECTOR is name:NAME, path:/ABSOLUTE/PATH, or inherit\n";
}

} // namespace libtmux::mcp::server
