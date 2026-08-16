// A parsed notification borrows from the notification it read, so parsing a
// temporary would hand back views into something already gone.

#include <libtmux/libtmux.hpp>

libtmux::Notification made();

auto rejected() { return libtmux::parse(made()); }
