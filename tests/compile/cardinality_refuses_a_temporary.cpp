// The reference `exactly_one` returns points into the range it was given, so
// a range that dies at the semicolon must be refused rather than dangle.

#include <vector>

#include <libtmux/libtmux.hpp>

std::vector<libtmux::Window> listed();

auto rejected() { return libtmux::exactly_one(listed()); }
