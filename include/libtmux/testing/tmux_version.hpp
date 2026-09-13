#pragma once

// The tmux the tests run against.
//
// Resolved at runtime from the same binary `ScopedTmuxServer` spawns, not
// stamped in by the build: an installed library cannot carry the consumer's
// tmux version, and a configure-time answer can name a different binary than
// the one that runs.
//
// No test framework is involved. `capabilities.hpp` layers GoogleTest skip
// macros on top; a suite on Catch2 or doctest uses this directly.

#include "libtmux/version.hpp"

#include <filesystem>
#include <string>

namespace libtmux::test {

// Resolved once per distinct binary and cached. A tmux that cannot be run or
// whose version cannot be parsed reports as the newest possible version, so
// version-gated tests run and fail rather than silently skipping.
[[nodiscard]] Version running_tmux(const std::filesystem::path& tmux_binary = "tmux");

// What `tmux -V` printed, verbatim, or "unknown".
[[nodiscard]] std::string
describe_running_tmux(const std::filesystem::path& tmux_binary = "tmux");

} // namespace libtmux::test
