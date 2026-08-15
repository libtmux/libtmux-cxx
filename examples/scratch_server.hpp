#pragma once

// Where these examples get a tmux to talk to.
//
// Always a server of their own, on a socket under a directory named for this
// workspace, killed on the way out. Never the server the reader is sitting in:
// an example creates and renames and kills things, and doing that to somebody's
// real session because they happened to run it from inside tmux is not a
// trade worth making for a slightly more realistic demonstration.
//
// `Server::from_env()` — the call that does reach the surrounding server — has
// an example of its own, which reads it rather than changing anything.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>

#include <libtmux/libtmux.hpp>

namespace example {

class ScratchServer {
public:
  static ScratchServer open() {
    // Named for the workspace, so a stray directory says where it came from,
    // and distinct from the fixture the test suite uses.
    std::string directory =
        (std::filesystem::temp_directory_path() / "libtmux-cxx-example-XXXXXX")
            .string();
    if (::mkdtemp(directory.data()) == nullptr) {
      std::perror("mkdtemp");
      std::exit(1);
    }
    const std::string socket = directory + "/socket";

    auto server = libtmux::Server::at_socket_path(socket);
    if (!server.has_value()) {
      std::fprintf(stderr, "%s\n", server.error().diagnostic.c_str());
      std::exit(1);
    }
    if (const auto started = server->new_session("example"); !started.has_value()) {
      std::fprintf(stderr, "%s\n", started.error().diagnostic.c_str());
      std::exit(1);
    }
    return ScratchServer{*std::move(server), std::move(directory)};
  }

  ~ScratchServer() {
    (void)server_.kill();
    std::error_code ignored;
    std::filesystem::remove_all(owned_, ignored);
  }

  ScratchServer(const ScratchServer&) = delete;
  ScratchServer& operator=(const ScratchServer&) = delete;
  ScratchServer(ScratchServer&&) = delete;
  ScratchServer& operator=(ScratchServer&&) = delete;

  [[nodiscard]] const libtmux::Server& get() const noexcept { return server_; }

private:
  ScratchServer(libtmux::Server server, std::string owned)
      : server_{std::move(server)}, owned_{std::move(owned)} {}

  libtmux::Server server_;
  // The private tree this server's socket lives in, removed with it.
  std::string owned_;
};

} // namespace example
