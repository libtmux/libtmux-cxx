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
  const auto windows = session.windows();
  const auto session_panes = session.panes();
  if (!windows.has_value()) {
    std::fprintf(stderr, "%s\n", windows.error().diagnostic.c_str());
    return 1;
  }
  if (!session_panes.has_value()) {
    std::fprintf(stderr, "%s\n", session_panes.error().diagnostic.c_str());
    return 1;
  }
  if (windows->size() != 1 || session_panes->size() != 1) {
    std::fputs("expected one window and pane\n", stderr);
    return 1;
  }
  const auto& window = windows->front();
  const auto panes = window.panes();
  if (!panes.has_value()) {
    std::fprintf(stderr, "%s\n", panes.error().diagnostic.c_str());
    return 1;
  }
  if (panes->size() != 1) {
    std::fputs("expected one window pane\n", stderr);
    return 1;
  }
  const auto owner_window = panes->front().window();
  const auto owner_session = panes->front().session();
  if (!owner_window.has_value()) {
    std::fprintf(stderr, "%s\n", owner_window.error().diagnostic.c_str());
    return 1;
  }
  if (!owner_session.has_value()) {
    std::fprintf(stderr, "%s\n", owner_session.error().diagnostic.c_str());
    return 1;
  }
  if (*owner_window != window || *owner_session != session ||
      panes->front() != session_panes->front()) {
    std::fputs("entity ownership does not match\n", stderr);
    return 1;
  }
  std::puts("api session -> one window -> one pane; owners match.");
  return 0;
}
