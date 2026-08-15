#pragma once

#include "libtmux/expected.hpp"

// Read a tmuxp document into a workspace description.
//
// This is where the YAML dependency lives, and it lives here because the
// library has no business knowing about configuration file formats: the core
// is dependency-free, and a consumer that wants one pays for it alone.
//
// The subset is what this can actually build: a session name, a directory
// under either spelling, setup commands, and windows carrying a layout,
// options, focus, and panes given as a command or as a mapping.
//
// tmuxp's own schema is larger, and a key outside the subset is refused by
// name rather than ignored. Reading a document as if it said less is the
// worse failure: a dropped `shell_command_before` builds panes that never
// activate their environment, and nothing about the session says why.

#include <expected>
#include <string>
#include <string_view>

#include "libtmux_consumers/workspace.hpp"

namespace libtmux::workspace {

struct ParseError {
  // The path through the document, as `windows[1].panes[0]`, so a message
  // points at the line a reader has to change.
  std::string where;
  std::string reason;
};

[[nodiscard]] libtmux::expected<Workspace, ParseError>
parse_tmuxp(std::string_view document);

} // namespace libtmux::workspace
