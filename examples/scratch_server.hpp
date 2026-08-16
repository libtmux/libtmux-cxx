#pragma once

// Where these examples get a tmux to talk to.
//
// Always a server of their own, on a socket under a directory named for this
// workspace, killed on the way out. Never the server the reader is sitting in:
// an example creates and renames and kills things, and doing that to somebody's
// real session because they happened to run it from inside tmux is not a
// trade worth making for a slightly more realistic demonstration.
//
// This used to be sixty lines of `mkdtemp` and socket bookkeeping copied out
// of the test suite. It is now four lines over `libtmux::testing`, the same
// fixture the suite runs on — which is the honest demonstration anyway: an
// example that showed a reader how to hand-roll a private server would be
// teaching them to reinvent something the package ships.
//
// `Server::from_env()` — the call that does reach the surrounding server — is
// deliberately not used here. It has no example: reaching outward is the one
// thing these programs must not do.

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
  // `suite` names the run in every socket path and stray directory it leaves.
  // The examples' own test harness overrides it through the environment so a
  // server started by a test is distinguishable from one a reader started by
  // running the example directly.
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

  // Declared first, destroyed last: the server handle must not outlive the
  // tmux it addresses.
  libtmux::test::ScopedTmuxServer fixture_;
  libtmux::Server server_;
};

} // namespace example
