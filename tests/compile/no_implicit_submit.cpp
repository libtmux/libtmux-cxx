#include <libtmux/libtmux.hpp>

// Asynchronous work must name the runtime that owns its threads and bound.
auto rejected(const libtmux::Server& server) {
  return server.submit({"display-message", "-p", "no runtime"});
}
