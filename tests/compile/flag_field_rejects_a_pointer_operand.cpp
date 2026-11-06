// A flag compares with a bool and nothing that merely converts to one. A string
// literal decays to a pointer, a pointer converts to bool, and the comparison
// once built a node testing for true.

#include <libtmux/libtmux.hpp>

auto rejected() { return libtmux::pane::active == "false"; }
