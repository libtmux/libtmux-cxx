// Finding things: typed fields, standard ranges, and cardinality that cannot
// dangle. Nothing here reaches tmux — the listing was taken once, up front.

#include <cstdio>
#include <ranges>
#include <string>
#include <vector>

#include <libtmux/libtmux.hpp>

#include "scratch_server.hpp"

int main() {
  const example::ScratchServer scratch = example::ScratchServer::open();
  const libtmux::Server& server = scratch.get();

  const auto sessions = server.sessions();
  if (!sessions.has_value()) {
    std::fprintf(stderr, "%s\n", sessions.error().diagnostic.c_str());
    return 1;
  }
  for (const char* name : {"editor", "logs", "build"}) {
    if (const auto made = sessions->at(0).new_window({.name = name});
        !made.has_value()) {
      std::fprintf(stderr, "%s\n", made.error().diagnostic.c_str());
      return 1;
    }
  }

  const auto windows = server.windows();
  if (!windows.has_value()) {
    std::fprintf(stderr, "%s\n", windows.error().diagnostic.c_str());
    return 1;
  }

  // A filter is a value, built from typed fields. `window::active.contains(…)`
  // would not compile: a flag has no string operations, and a size compares as
  // a number.
  const auto interesting =
      libtmux::window::name.starts_with("e") || libtmux::window::name == "logs";

  auto matched = *windows | libtmux::matching(interesting);
  for (const libtmux::Window& window : matched) {
    std::printf("matched %s\n", std::string{window.name()}.c_str());
  }

  // Plain ranges work too — a listing is a vector.
  const auto wide = std::ranges::count_if(
      *windows, [](const libtmux::Window& window) { return window.width() > 40; });
  std::printf("%lld window(s) wider than 40 columns\n", static_cast<long long>(wide));

  // Asking for one says which way it went wrong. Both helpers take a named
  // range: the answer refers into it, so a temporary is a compile error.
  auto logs = *windows | libtmux::matching(libtmux::window::name == "logs");
  if (const auto only = libtmux::exactly_one(logs); only.has_value()) {
    std::printf("exactly one logs window: %s\n", std::string{only->get().id()}.c_str());
  } else {
    std::printf("no single logs window: %s\n",
                std::string{libtmux::to_string(only.error())}.c_str());
  }

  auto missing = *windows | libtmux::matching(libtmux::window::name == "absent");
  const auto none = libtmux::exactly_one(missing);
  std::printf("looking for one that is not there: %s\n",
              std::string{libtmux::to_string(none.error())}.c_str());
  return 0;
}
