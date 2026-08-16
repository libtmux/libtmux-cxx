#pragma once

// Where tmux would put the socket a selector names.
//
// tmux's own rule, from `make_label` in tmux.c: take the first of
// `$TMUX_TMPDIR` and `/tmp` that resolves, append `tmux-<uid>`, and put the
// label under that — "default" when the selector names none. The directory is
// resolved the way tmux resolves it, with realpath, so two selectors differing
// only by a symlink name one server.
//
// Two things need this, and neither could be had from the argv alone.
//
// A control client is launched against a path, so a server selected by `-L` or
// by nothing at all had no path to hand it and could not use the faster
// transport at all.
//
// And a command combining two entities has to know whether they came from one
// tmux. Ids are numbered per server: `%1` on one socket names a different pane
// on another, so `pane_a.swap_with(pane_b)` across two servers ran against
// `pane_a`'s and found some unrelated pane there. Comparing the argv instead
// would call `-L work` and `-S <the path that resolves to>` different servers
// when they are the same one.

#include "libtmux/abi.hpp"

#include <optional>
#include <string>
#include <vector>

LIBTMUX_NAMESPACE_BEGIN
namespace detail {

// Empty when the selector is one this does not understand, or when even `/tmp`
// will not resolve. Callers treat that as "no identity" rather than guessing:
// an identity nobody can confirm must not make two servers look like one.
[[nodiscard]] std::optional<std::string>
resolved_socket_path(const std::vector<std::string>& selector);

} // namespace detail
LIBTMUX_NAMESPACE_END
