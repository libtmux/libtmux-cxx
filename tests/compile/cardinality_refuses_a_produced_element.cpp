// A range that produces its elements on demand has nothing for the answer to
// refer to: the element dies with the call that made it.

#include <ranges>
#include <vector>

#include <libtmux/libtmux.hpp>

std::vector<libtmux::Pane> listed();

auto rejected() {
  static std::vector<libtmux::Pane> panes = listed();
  auto copies =
      panes | std::views::transform([](const libtmux::Pane& pane) { return pane; });
  return libtmux::exactly_one(copies);
}
