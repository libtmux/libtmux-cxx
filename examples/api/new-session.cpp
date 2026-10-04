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

  const auto made = server->new_session(libtmux::NewSessionOptions{
      .name = "work",
      .first_window_name = "main",
      .shell_command = "exec cat",
  });
  if (!made.has_value()) {
    std::fprintf(stderr, "%s\n", made.error().diagnostic.c_str());
    return 1;
  }
  const auto window = made->active_window();
  const auto sessions = server->sessions();
  if (!window.has_value()) {
    std::fprintf(stderr, "%s\n", window.error().diagnostic.c_str());
    return 1;
  }
  if (!sessions.has_value()) {
    std::fprintf(stderr, "%s\n", sessions.error().diagnostic.c_str());
    return 1;
  }
  if (made->name() != "work" || sessions->size() != 2 ||
      window->name() != "main" || window->session_id() != made->id()) {
    std::fputs("created session or first window does not match\n", stderr);
    return 1;
  }
  std::puts("Created work with the main window; two sessions are available.");
  return 0;
}
