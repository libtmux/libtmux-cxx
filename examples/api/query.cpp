#include <cstdio>
#include <ranges>
#include <string>

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
  for (const char* name : {"editor", "logs"}) {
    const auto made = sessions->front().new_window(
        libtmux::NewWindowOptions{.name = name, .shell_command = "exec cat"});
    if (!made.has_value()) {
      std::fprintf(stderr, "%s\n", made.error().diagnostic.c_str());
      return 1;
    }
  }
  const auto windows = server->windows();
  if (!windows.has_value()) {
    std::fprintf(stderr, "%s\n", windows.error().diagnostic.c_str());
    return 1;
  }
  const auto wanted = libtmux::window::name.starts_with("edit") ||
                      libtmux::window::name == "logs";
  auto matches = *windows | libtmux::matching(wanted);
  const auto editor = libtmux::exactly_one_owned(
      *windows | libtmux::matching(libtmux::window::name == "editor"));
  auto absent = *windows | libtmux::matching(libtmux::window::name == "absent");
  const auto missing = libtmux::exactly_one(absent);
  const auto ambiguous = libtmux::exactly_one(matches);
  if (!editor.has_value() || std::ranges::distance(matches) != 2 ||
      missing.has_value() ||
      missing.error() != libtmux::CardinalityError::none_matched ||
      ambiguous.has_value() ||
      ambiguous.error() != libtmux::CardinalityError::several_matched) {
    std::fputs("unexpected query cardinality\n", stderr);
    return 1;
  }
  std::printf("Selected %s; two matches; missing and ambiguous are distinct.\n",
              std::string{editor->name()}.c_str());
  return 0;
}
