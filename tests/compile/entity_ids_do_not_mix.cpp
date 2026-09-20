// A window id is not a pane id. They are both text tmux spells with a prefix,
// and while both were `std::string_view` this comparison built and could never
// be true.

#include <libtmux/libtmux.hpp>

bool rejected(const libtmux::Pane& pane, const libtmux::Window& window) {
  return pane.id() == window.id();
}
