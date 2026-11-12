// A character is not a number: 'x' would be compared as its code, 120.

#include <libtmux/libtmux.hpp>

auto rejected() { return libtmux::pane::width == 'x'; }
