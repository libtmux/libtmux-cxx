// A numeric field holds a long long. An unsigned type as wide as that can hold
// a value it cannot, and the largest one wrapped to -1.

#include <climits>
#include <libtmux/libtmux.hpp>

auto rejected() { return libtmux::pane::width == ULLONG_MAX; }
