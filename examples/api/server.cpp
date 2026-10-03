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

  // Copying the handle keeps the same connection; it does not start another server.
  const libtmux::Server copy = *server;
  const auto sessions = copy.sessions();
  if (!sessions.has_value()) {
    std::fprintf(stderr, "%s\n", sessions.error().diagnostic.c_str());
    return 1;
  }
  if (copy.socket_path() != fixture->socket_path().string() || sessions->size() != 1 ||
      sessions->front().name() != "api") {
    std::fputs("unexpected server or session\n", stderr);
    return 1;
  }
  std::puts("Connected to the private api session.");
  return 0;
}
