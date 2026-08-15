// `Pane::capture` returns its output by value, so framing that value directly
// hands back views into a string that dies at the semicolon — the exact
// spelling the entity header used to recommend.

#include <libtmux/libtmux.hpp>

libtmux::expected<std::string, libtmux::CommandFailure> captured();

auto rejected() { return libtmux::capture_lines(*captured()); }
