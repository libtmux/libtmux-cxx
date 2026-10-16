// The join borrows the rows it reads, so it must refuse a listing that will
// not outlive the predicate built from it.

#include <vector>

#include <libtmux/libtmux.hpp>

std::vector<libtmux::Window> listed();

auto rejected() {
#ifdef LIBTMUX_TEST_PARENT_JOIN
  return libtmux::parent_of<libtmux::Pane, libtmux::Window>(
      listed(), libtmux::pane::window_id, libtmux::window::id);
#else
  return libtmux::children_of<libtmux::Session, libtmux::Window>(
      listed(), libtmux::window::session_id, libtmux::session::id);
#endif
}
