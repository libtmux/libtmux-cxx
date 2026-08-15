// A string field has no ordering here: tmux renders a name and a number the
// same way, and ordering names as text is a different question from ordering
// numbers, so the type does not pretend to answer it.

#include <libtmux/libtmux.hpp>

auto rejected() { return libtmux::window::name > 5; }
