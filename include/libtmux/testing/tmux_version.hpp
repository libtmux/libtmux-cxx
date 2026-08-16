#pragma once

// Which tmux the tests are actually running against.
//
// The library supports 3.2a and newer, and across that range tmux does not
// answer identically: some capabilities arrived later, and a few releases
// answered a question wrongly and a later one fixed it. A test asserting such
// a behaviour is asserting something about the tmux underneath, so it has to
// be able to ask which one that is.
//
// Resolved once, at runtime, from the same binary `ScopedTmuxServer` will
// spawn — not stamped in by the build. The build-time stamp was a hazard: it
// recorded whatever `find_program` saw at configure time, which is not
// necessarily what runs, and it made the fixture impossible to install (a
// shipped library cannot carry the consumer's tmux version). Asking the
// binary the fixture is about to launch cannot disagree with the binary the
// fixture launches.
//
// No test framework is involved. `capabilities.hpp` layers GoogleTest skip
// macros on top for suites that want them; a suite on Catch2 or doctest uses
// this directly.

#include "libtmux/version.hpp"

#include <filesystem>
#include <string>

namespace libtmux::test {

// The version of `tmux_binary`, resolved once per distinct binary and cached.
//
// An unreadable or unrunnable tmux reports as the newest possible version
// rather than the oldest. A test that then fails says something true about
// this tmux; a test wrongly skipped says nothing at all, and that is the worse
// of the two.
[[nodiscard]] const Version&
running_tmux(const std::filesystem::path& tmux_binary = "tmux");

// What `tmux -V` printed, verbatim, for putting in a skip message.
[[nodiscard]] std::string
describe_running_tmux(const std::filesystem::path& tmux_binary = "tmux");

} // namespace libtmux::test
