#pragma once

// Where these examples get a tmux to talk to.
//
// Inside tmux, they use the server they are running in — which is what a
// reader wants to see, because it is what their own program will do. Outside
// one, they start a private server on a socket of their own and kill it on the
// way out, so an example is runnable anywhere and leaves nothing behind.

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
    if (auto inherited = libtmux::Server::from_env(); inherited.has_value()) {
      return ScratchServer{*std::move(inherited), {}};
    }

    std::string directory =
        (std::filesystem::temp_directory_path() / "libtmux-example-XXXXXX").string();
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
    if (owned_.empty()) {
      return;
    }
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
  // Empty when the server was inherited rather than started here.
  std::string owned_;
};

} // namespace example
