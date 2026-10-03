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
  const auto windows = server->windows();
  const auto panes = server->panes();
  const auto clients = server->clients();
  if (!sessions.has_value()) {
    std::fprintf(stderr, "%s\n", sessions.error().diagnostic.c_str());
    return 1;
  }
  if (!windows.has_value()) {
    std::fprintf(stderr, "%s\n", windows.error().diagnostic.c_str());
    return 1;
  }
  if (!panes.has_value()) {
    std::fprintf(stderr, "%s\n", panes.error().diagnostic.c_str());
    return 1;
  }
  if (!clients.has_value()) {
    std::fprintf(stderr, "%s\n", clients.error().diagnostic.c_str());
    return 1;
  }
  if (sessions->size() != 1 || windows->size() != 1 || panes->size() != 1 ||
      !clients->empty()) {
    std::fputs("unexpected detached server contents\n", stderr);
    return 1;
  }
  std::printf("sessions=%zu windows=%zu panes=%zu clients=%zu\n", sessions->size(),
              windows->size(), panes->size(), clients->size());
  return 0;
}
