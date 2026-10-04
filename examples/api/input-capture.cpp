#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ranges>

#include <libtmux/libtmux.hpp>
#include <libtmux/testing/scoped_server.hpp>

int main() {
  // The fixture owns this private server and cleans it up on every return path.
  auto fixture = libtmux::test::ScopedTmuxServer::start({
      .session_name = "api",
      .socket_namespace = libtmux::test::SocketNamespace::consumer("api"),
  });
  if (!fixture.has_value()) {
    std::fprintf(stderr, "%s\n", fixture.error().c_str());
    return 1;
  }
  auto server = libtmux::Server::at_socket_path(fixture->socket_path());
  if (!server.has_value()) {
    std::fprintf(stderr, "%s\n", server.error().diagnostic.c_str());
    return 1;
  }

  const auto sessions = server->sessions();
  if (!sessions.has_value()) {
    std::fprintf(stderr, "%s\n", sessions.error().diagnostic.c_str());
    return 1;
  }
  if (sessions->size() != 1) {
    std::fputs("expected one session\n", stderr);
    return 1;
  }
  const auto window = sessions->front().new_window(libtmux::NewWindowOptions{
      .name = "reader",
      .shell_command = R"(exec sh -c '
        stty -echo
        printf "ready\n"
        IFS= read -r line
        printf "got:%s\n" "$line"
        exec cat >/dev/null
      ')",
  });
  if (!window.has_value()) {
    std::fprintf(stderr, "%s\n", window.error().diagnostic.c_str());
    return 1;
  }
  const auto pane = window->active_pane();
  if (!pane.has_value()) {
    std::fprintf(stderr, "%s\n", pane.error().diagnostic.c_str());
    return 1;
  }
  const auto ready =
      pane->wait_for_text("ready", {.timeout = std::chrono::seconds{20}});
  if (!ready.has_value()) {
    std::fprintf(stderr, "%s\n", ready.error().diagnostic.c_str());
    return 1;
  }
  if (!ready->matched) {
    std::fputs("reader did not become ready\n", stderr);
    return 1;
  }
  const auto text = pane->send_text("hello ");
  if (!text.has_value()) {
    std::fprintf(stderr, "%s\n", text.error().diagnostic.c_str());
    return 1;
  }
  const auto line = pane->send_line("C++");
  if (!line.has_value()) {
    std::fprintf(stderr, "%s\n", line.error().diagnostic.c_str());
    return 1;
  }
  const auto answered =
      pane->wait_for_text("got:hello C++", {.timeout = std::chrono::seconds{20}});
  if (!answered.has_value()) {
    std::fprintf(stderr, "%s\n", answered.error().diagnostic.c_str());
    return 1;
  }
  if (!answered->matched) {
    std::fputs("reader did not confirm the input\n", stderr);
    return 1;
  }
  const auto captured = pane->capture();
  if (!captured.has_value()) {
    std::fprintf(stderr, "%s\n", captured.error().diagnostic.c_str());
    return 1;
  }
  // The captured string outlives the line views returned by capture_lines.
  const auto lines = libtmux::capture_lines(*captured);
  if (std::ranges::find(lines, "got:hello C++") == std::ranges::end(lines)) {
    std::fputs("captured output does not contain the response\n", stderr);
    return 1;
  }
  std::puts("got:hello C++");
  return 0;
}
