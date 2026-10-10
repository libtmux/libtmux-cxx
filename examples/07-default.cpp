#include <exception>
#include <iostream>
#include <string>
#include <unistd.h>

#include <libtmux/libtmux.hpp>

int main() {
  try {
    auto server = libtmux::Server::at_default();
    if (!server) {
      std::cerr << server.error().diagnostic << '\n';
      return 1;
    }
    auto session = libtmux::own_session(
        *server, {.name = "libtmux-example-" + std::to_string(::getpid())});
    if (!session) {
      std::cerr << session.error().primary.diagnostic << '\n';
      if (session.error().rollback) {
        std::cerr << "session rollback: " << session.error().rollback->diagnostic
                  << '\n';
      }
      return 1;
    }
    const auto result =
        libtmux::with_owned(*session,
                            [](const libtmux::Session& value)
                                -> libtmux::expected<void, libtmux::CommandFailure> {
                              std::cout << "created " << value.id().value() << '\n';
                              const auto windows = value.windows();
                              if (!windows)
                                return libtmux::unexpected(windows.error());
                              std::cout << windows->size() << " window(s)\n";
                              return {};
                            });
    if (result.body_failure)
      std::cerr << result.body_failure->diagnostic << '\n';
    if (result.cleanup.failure) {
      std::cerr << "session cleanup: " << result.cleanup.failure->diagnostic << '\n';
    }
    if (result.body_exception || result.cleanup.exception) {
      std::cerr << "example body or cleanup threw\n";
    }
    return result.ok() ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
