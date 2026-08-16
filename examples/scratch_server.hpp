#pragma once

// Where these examples get a tmux to talk to: a private server of their own,
// killed on the way out, never the one the reader is sitting in.
//
// `libtmux::testing` does the work. An example that hand-rolled a private
// server would be teaching a reader to reinvent what the package ships.
//
// `Server::from_env()` is deliberately absent — reaching the surrounding
// server is what these programs must not do.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

#include <libtmux/libtmux.hpp>
#include <libtmux/testing/scoped_server.hpp>

namespace example {

class ScratchServer {
public:
  // `LIBTMUX_EXAMPLE_NAMESPACE` overrides `suite`, so the examples' own test
  // harness can label the servers a test run leaves behind.
  static ScratchServer open(std::string_view suite = "example") {
    const char* const named = std::getenv("LIBTMUX_EXAMPLE_NAMESPACE");
    auto fixture = libtmux::test::ScopedTmuxServer::start({
        .session_name = "example",
        .socket_namespace = libtmux::test::SocketNamespace::consumer(
            named != nullptr && named[0] != '\0' ? named : suite),
    });
    if (!fixture.has_value()) {
      std::fprintf(stderr, "%s\n", fixture.error().c_str());
      std::exit(1);
    }

    auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
    if (!server.has_value()) {
      std::fprintf(stderr, "%s\n", server.error().diagnostic.c_str());
      std::exit(1);
    }
    return ScratchServer{*std::move(fixture), *std::move(server)};
  }

  ScratchServer(const ScratchServer&) = delete;
  ScratchServer& operator=(const ScratchServer&) = delete;
  ScratchServer(ScratchServer&&) = delete;
  ScratchServer& operator=(ScratchServer&&) = delete;

  [[nodiscard]] const libtmux::Server& get() const noexcept { return server_; }

private:
  ScratchServer(libtmux::test::ScopedTmuxServer fixture, libtmux::Server server)
      : fixture_{std::move(fixture)}, server_{std::move(server)} {}

  // Declared first, so it is destroyed last: the handle must not outlive the
  // server it addresses.
  libtmux::test::ScopedTmuxServer fixture_;
  libtmux::Server server_;
};

} // namespace example
