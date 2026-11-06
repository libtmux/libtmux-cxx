// A bool is not a number: `true` would be compared as 1.

#include <libtmux/libtmux.hpp>

auto rejected() { return libtmux::pane::width == true; }
