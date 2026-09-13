#pragma once

#include "libtmux/abi.hpp"
#include "libtmux/command.hpp"
#include "libtmux/expected.hpp"
#include <cstddef>
#include <string_view>

LIBTMUX_NAMESPACE_BEGIN

struct LayoutRequest {
  std::string_view layout;
  // Planned panes that need cells. Use one when current topology is unknown.
  std::size_t minimum_panes{1};
};

struct LayoutFailure {
  // Index in the supplied batch, including failures while querying its daemon.
  std::size_t index{};
  CommandFailure cause;
};

// Check names, v1 saved-layout checksum, v1/v2 tree grammar and minimum cell count
// without I/O. Names must be valid on at least one supported tmux version;
// Server::validate_layouts resolves version-dependent names and requires tmux
// 3.9 (including next-3.9) or newer for v2 JSON layouts. Saved input, including
// floating-pane metadata, is retained unchanged. The v2 reader follows tmux's
// restricted JSON syntax. Geometry and resizing remain tmux's responsibility. Empty
// layouts are invalid.
[[nodiscard]] expected<void, CommandFailure>
validate_layout(std::string_view layout, std::size_t minimum_panes = 1);

LIBTMUX_NAMESPACE_END
