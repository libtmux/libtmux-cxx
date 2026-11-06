// A numeric field holds an integer, so a fractional operand would be truncated
// to a different number before it was compared.

#include <libtmux/libtmux.hpp>

auto rejected() { return libtmux::pane::width > 80.9; }
