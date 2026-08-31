#pragma once

// Where these examples get a tmux to talk to: a private server of their own,
// killed on the way out, never the one the reader is sitting in.
//
// `libtmux::testing` does the work. An example that hand-rolled a private
// server would be teaching a reader to reinvent what the package ships.
//
// `Server::from_env()` is deliberately absent — reaching the surrounding
// server is what these programs must not do.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <unistd.h>

#include <libtmux/libtmux.hpp>
#include <libtmux/testing/scoped_server.hpp>

namespace example {

class ScratchServer {
public:
  // `LIBTMUX_EXAMPLE_NAMESPACE` overrides `suite`, so the examples' own test
  // harness can label the servers a test run leaves behind.
  static ScratchServer open(std::string_view suite = "example") {
    const char* const named = std::getenv("LIBTMUX_EXAMPLE_NAMESPACE");
    auto fixture = libtmux::test::ScopedTmuxServer::start({
        .session_name = "example",
        .socket_namespace = libtmux::test::SocketNamespace::consumer(
            named != nullptr && named[0] != '\0' ? named : suite),
    });
    if (!fixture.has_value()) {
      std::fprintf(stderr, "%s\n", fixture.error().c_str());
      std::exit(1);
    }

    auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
    if (!server.has_value()) {
      std::fprintf(stderr, "%s\n", server.error().diagnostic.c_str());
      std::exit(1);
    }
    return ScratchServer{*std::move(fixture), *std::move(server)};
  }

  static ScratchServer open_or_borrow_arena(std::string_view expected_artifact,
                                            std::string_view suite = "example") {
    const char* const descriptor = std::getenv("LIBTMUX_ARENA_DESCRIPTOR");
    if (descriptor == nullptr || descriptor[0] == '\0') {
      return open(suite);
    }

    const char* const artifact = std::getenv("LIBTMUX_ARENA_ARTIFACT");
    const char* const socket_path = std::getenv("LIBTMUX_SOCKET_PATH");
    const char* const tmux_binary = std::getenv("LIBTMUX_TMUX_BIN");
    const auto present = [](const char* value) {
      return value != nullptr && value[0] != '\0';
    };
    if (!present(artifact) || !present(socket_path) || !present(tmux_binary) ||
        std::string_view{artifact} != expected_artifact) {
      std::fprintf(stderr, "incomplete or mismatched arena contract\n");
      std::exit(1);
    }
    if (!arena_tmux_binary_is_on_path(tmux_binary)) {
      std::fprintf(stderr, "arena tmux binary is not PATH's tmux\n");
      std::exit(1);
    }

    auto server = libtmux::Server::at_socket_path(socket_path);
    if (!server.has_value()) {
      std::fprintf(stderr, "%s\n", server.error().diagnostic.c_str());
      std::exit(1);
    }
    return ScratchServer{std::filesystem::path{socket_path}, *std::move(server)};
  }

  ScratchServer(const ScratchServer&) = delete;
  ScratchServer& operator=(const ScratchServer&) = delete;
  ScratchServer(ScratchServer&&) = delete;
  ScratchServer& operator=(ScratchServer&&) = delete;

  [[nodiscard]] const libtmux::Server& get() const noexcept { return server_; }

  [[nodiscard]] bool borrows_server() const noexcept { return !fixture_.has_value(); }

  // For the one example that opens a control connection, which is addressed by
  // socket rather than through `Server`.
  [[nodiscard]] std::filesystem::path socket_path() const { return socket_path_; }

private:
  static bool arena_tmux_binary_is_on_path(const char* tmux_binary) {
    std::filesystem::path requested{tmux_binary};
    if (!requested.is_absolute() || requested.filename() != "tmux") {
      return false;
    }
    std::error_code error;
    requested = std::filesystem::canonical(requested, error);
    if (error) {
      return false;
    }

    const char* const path = std::getenv("PATH");
    if (path == nullptr) {
      return false;
    }
    for (std::string_view entries{path};;) {
      const std::size_t separator = entries.find(':');
      const std::string_view directory = entries.substr(0, separator);
      const std::filesystem::path candidate =
          std::filesystem::path{directory.empty() ? "." : directory} / "tmux";
      error.clear();
      if (std::filesystem::is_regular_file(candidate, error) && !error &&
          ::access(candidate.c_str(), X_OK) == 0) {
        const auto resolved = std::filesystem::canonical(candidate, error);
        return !error && resolved == requested;
      }
      if (separator == std::string_view::npos) {
        return false;
      }
      entries.remove_prefix(separator + 1U);
    }
  }

  ScratchServer(libtmux::test::ScopedTmuxServer fixture, libtmux::Server server)
      : fixture_{std::move(fixture)}, socket_path_{fixture_->socket_path()},
        server_{std::move(server)} {}

  ScratchServer(std::filesystem::path socket_path, libtmux::Server server)
      : socket_path_{std::move(socket_path)}, server_{std::move(server)} {}

  // Declared first, so it is destroyed last: the handle must not outlive the
  // server it addresses.
  std::optional<libtmux::test::ScopedTmuxServer> fixture_;
  std::filesystem::path socket_path_;
  libtmux::Server server_;
};

} // namespace example
