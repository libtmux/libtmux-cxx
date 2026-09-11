#pragma once

// Where these examples get a tmux to talk to: a private server of their own,
// killed on the way out, never the one the reader is sitting in.
//
// `libtmux::testing` does the work. An example that hand-rolled a private
// server would be teaching a reader to reinvent what the package ships.
//
// `Server::from_env()` is deliberately absent — reaching the surrounding
// server is what these programs must not do.

#include <charconv>
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

  // Proof, for the documentation arena, that an example ran against the
  // server it was handed rather than one of its own: the reported socket path
  // matches what was requested, the pid is real, and the challenge the arena
  // planted is readable back. Prints nothing and returns nonzero if any of
  // that does not hold. Callers print this only when `borrows_server()`.
  [[nodiscard]] int print_arena_evidence(std::string_view artifact) const {
    const auto identity = server_.expand("#{pid}\t#{socket_path}");
    if (!identity.has_value()) {
      std::fprintf(stderr, "%s\n", identity.error().diagnostic.c_str());
      return 1;
    }

    const std::size_t separator = identity->find('\t');
    if (separator == std::string::npos) {
      std::fprintf(stderr, "invalid arena server identity\n");
      return 1;
    }
    int server_pid = 0;
    const char* const pid_end = identity->data() + separator;
    const auto parsed = std::from_chars(identity->data(), pid_end, server_pid);
    const std::string reported_socket_path = identity->substr(separator + 1U);
    if (parsed.ec != std::errc{} || parsed.ptr != pid_end || server_pid <= 0 ||
        reported_socket_path != socket_path()) {
      std::fprintf(stderr, "invalid arena server identity\n");
      return 1;
    }

    const auto global_options = server_.global_options();
    if (!global_options.has_value()) {
      std::fprintf(stderr, "%s\n", global_options.error().diagnostic.c_str());
      return 1;
    }
    std::string_view challenge;
    for (const libtmux::OptionEntry& option : *global_options) {
      if (option.name == "@libtmux_arena_challenge" && !option.index.has_value()) {
        challenge = option.value;
        break;
      }
    }
    if (challenge.empty()) {
      std::fprintf(stderr, "arena challenge is missing\n");
      return 1;
    }

    std::string evidence{"LIBTMUX_ARENA_EVIDENCE={\"schema\":1,\"server_pid\":"};
    evidence += std::to_string(server_pid);
    evidence += ",\"socket_path\":";
    append_json_string(evidence, reported_socket_path);
    evidence += ",\"challenge\":";
    append_json_string(evidence, challenge);
    evidence += ",\"artifact\":";
    append_json_string(evidence, artifact);
    evidence += "}\n";
    std::fputs(evidence.c_str(), stdout);
    return 0;
  }

private:
  static void append_json_string(std::string& output, std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    output.push_back('\"');
    for (const char value_character : value) {
      const unsigned char character = static_cast<unsigned char>(value_character);
      switch (character) {
      case '\"':
        output += "\\\"";
        break;
      case '\\':
        output += "\\\\";
        break;
      case '\b':
        output += "\\b";
        break;
      case '\f':
        output += "\\f";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        if (character < 0x20U || character >= 0x80U) {
          output += "\\u00";
          output.push_back(hex[character >> 4U]);
          output.push_back(hex[character & 0x0fU]);
        } else {
          output.push_back(static_cast<char>(character));
        }
      }
    }
    output.push_back('\"');
  }

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
