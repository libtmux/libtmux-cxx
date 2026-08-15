// A flag field must not offer string operations: tmux renders it as "1" or
// "0", and comparing that text is a mistake the type system can catch.

#include <libtmux/libtmux.hpp>

auto rejected() { return libtmux::pane::active.starts_with("1"); }
