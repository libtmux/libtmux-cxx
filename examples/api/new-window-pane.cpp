#include <cstdio>

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
  const auto& session = sessions->front();
  const auto before = session.active_window();
  if (!before.has_value()) {
    std::fprintf(stderr, "%s\n", before.error().diagnostic.c_str());
    return 1;
  }
  const auto window = session.new_window(
      libtmux::NewWindowOptions{.name = "editor", .shell_command = "exec cat"});
  if (!window.has_value()) {
    std::fprintf(stderr, "%s\n", window.error().diagnostic.c_str());
    return 1;
  }
  const auto first = window->active_pane();
  if (!first.has_value()) {
    std::fprintf(stderr, "%s\n", first.error().diagnostic.c_str());
    return 1;
  }
  const auto second = first->split(
      libtmux::SplitOptions{.horizontal = true, .shell_command = "exec cat"});
  if (!second.has_value()) {
    std::fprintf(stderr, "%s\n", second.error().diagnostic.c_str());
    return 1;
  }
  const auto panes = window->panes();
  const auto after = session.active_window();
  if (!panes.has_value()) {
    std::fprintf(stderr, "%s\n", panes.error().diagnostic.c_str());
    return 1;
  }
  if (!after.has_value()) {
    std::fprintf(stderr, "%s\n", after.error().diagnostic.c_str());
    return 1;
  }
  if (panes->size() != 2 || *before != *after ||
      first->window_id() != second->window_id() || *first == *second) {
    std::fputs("created panes or selection do not match\n", stderr);
    return 1;
  }
  std::puts("Created editor with two panes; the active window is unchanged.");
  return 0;
}
