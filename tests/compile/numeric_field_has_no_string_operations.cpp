// A numeric field compares as a number. Offering `contains` on a width would
// invite comparing rendered text, which is how "9" ends up after "10".

#include <libtmux/libtmux.hpp>

auto rejected() { return libtmux::pane::width.contains("8"); }
