// Command quickstart types a command into tmux and waits for its answer.
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>

#include <libtmux/libtmux.hpp>

namespace {

// Stops the server on every path out of `run`.
struct StopOnExit {
  const libtmux::Server& server;
  ~StopOnExit() { (void)server.run({"kill-server"}); }
};

int run(const std::filesystem::path& socket) {
  // A server on a socket of its own, with no configuration file, so this
  // neither touches nor is changed by a tmux you already use.
  const auto server = libtmux::Server::startable_at_socket_path(
      socket, std::filesystem::path{"/dev/null"});
  if (!server.has_value()) {
    std::cerr << std::format("{}\n", server.error());
    return 1;
  }
  const StopOnExit stop{*server};

  // The first session starts the server. Its pane runs `sh`, so the prompt
  // does not depend on your login shell.
  const auto session =
      server->new_session({.name = "demo", .shell_command = "exec sh"});
  const auto pane = session.and_then(
      [](const libtmux::Session& created) { return created.active_pane(); });
  if (!pane.has_value()) {
    std::cerr << std::format("{}\n", pane.error());
    return 1;
  }

  // The terminal echoes the typed line, so wait for the answer, which the
  // typed line never contains.
  (void)pane->send_line("echo \"6 x 7 = $((6 * 7))\"");
  const auto answered =
      pane->wait_for_text("6 x 7 = 42", {.timeout = std::chrono::seconds{5}});
  if (!answered.has_value() || !answered->matched) {
    std::cerr << "the pane never printed the answer\n";
    return 1;
  }
  std::cout << "the pane answered 6 x 7 = 42\n";
  return 0;
}

} // namespace

int main() {
  char directory[] = "/tmp/libtmux-quickstart-XXXXXX";
  if (mkdtemp(directory) == nullptr) {
    return 1;
  }
  const int status = run(std::filesystem::path{directory} / "tmux.sock");
  std::filesystem::remove_all(directory);
  return status;
}
