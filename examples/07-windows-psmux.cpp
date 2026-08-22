// The Windows preview starts from an existing psmux session. Typed creation
// fails closed because psmux 3.3.7 cannot identify the winner of a create race.

#include <cstdio>
#include <string>

#include <libtmux/libtmux.hpp>

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s SOCKET_NAME SESSION_NAME\n", argv[0]);
    return 2;
  }

  auto opened = libtmux::Server::at_socket_name(argv[1]);
  if (!opened.has_value()) {
    std::fprintf(stderr, "%s\n", opened.error().diagnostic.c_str());
    return 1;
  }
  const auto capabilities = opened->capabilities();
  if (capabilities.implementation != libtmux::ServerImplementation::psmux ||
      !capabilities.supports(libtmux::ServerFeature::exact_inspection)) {
    std::fprintf(stderr, "this example needs the psmux inspection preview\n");
    return 1;
  }
  auto session = opened->session(argv[2]);
  if (!session.has_value()) {
    std::fprintf(stderr, "%s\n", session.error().diagnostic.c_str());
    return 1;
  }

  const auto sessions = opened->sessions();
  const auto windows = session->windows();
  const auto panes = session->panes();
  if (!sessions.has_value() || !windows.has_value() || !panes.has_value()) {
    const auto& failure = !sessions.has_value()  ? sessions.error()
                          : !windows.has_value() ? windows.error()
                                                 : panes.error();
    std::fprintf(stderr, "%s\n", failure.diagnostic.c_str());
    return 1;
  }
  std::printf("%s: %zu session, %zu window, %zu pane\n",
              std::string{session->name()}.c_str(), sessions->size(), windows->size(),
              panes->size());

  return 0;
}
