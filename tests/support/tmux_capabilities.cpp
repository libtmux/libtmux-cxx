#include "support/tmux_capabilities.hpp"

#include "libtmux/version.hpp"

#include <string>
#include <string_view>

#ifndef LIBTMUX_TEST_TMUX_VERSION
#error "LIBTMUX_TEST_TMUX_VERSION must name the tmux the suite resolved"
#endif

namespace libtmux::test {
namespace {

// The build already asked the resolved binary for its version, so take that
// answer rather than asking again: a second `tmux -V` could find a different
// tmux on PATH than the one the tests run.
Version resolve_running_tmux() {
  const auto parsed = parse_version(LIBTMUX_TEST_TMUX_VERSION);
  if (parsed.has_value()) {
    return *parsed;
  }
  // Unreadable version: report the newest possible, so nothing is skipped.
  // A test that then fails says something true about this tmux; a test wrongly
  // skipped says nothing at all, and that is the worse of the two.
  return Version{.unbounded = true};
}

} // namespace

const Version& running_tmux() {
  static const Version resolved = resolve_running_tmux();
  return resolved;
}

std::string describe_running_tmux() { return std::string{LIBTMUX_TEST_TMUX_VERSION}; }

} // namespace libtmux::test
