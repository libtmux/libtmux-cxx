#pragma once

#include "libtmux/capture.hpp"

#include <optional>

LIBTMUX_NAMESPACE_BEGIN
namespace detail {

// Count from the bottom: joined wraps above the cursor shorten the capture.
struct PaneCursor {
  std::size_t row{};
  std::size_t pane_height{};
};

[[nodiscard]] inline bool
output_confirms(std::string_view captured, std::string_view wanted,
                const std::vector<std::string>& sent = {},
                std::optional<PaneCursor> cursor = std::nullopt,
                bool input_pending = true) {
  if (wanted.empty()) {
    return false;
  }
  if (!cursor.has_value()) {
    return libtmux::output_confirms(captured, wanted, sent);
  }
  std::string_view active_row;
  std::string_view above;
  if (!sent.empty() && input_pending) {
    const std::vector<std::string_view> lines = capture_lines(captured);
    const auto joined = static_cast<long long>(lines.size());
    const auto index = joined - static_cast<long long>(cursor->pane_height) +
                       static_cast<long long>(cursor->row);
    if (index >= 0 && index < joined) {
      const auto row = static_cast<std::size_t>(index);
      active_row = lines[row];
      above = captured.substr(
          0, static_cast<std::size_t>(lines[row].data() - captured.data()));
    }
    // A position outside the captured range excludes nothing.
  }

  if (active_row.find(wanted) != std::string_view::npos &&
      above.find(wanted) == std::string_view::npos) {
    return false;
  }

  std::string masked{captured};
  for (const std::string& entry : sent) {
    if (!entry.empty()) {
      masked = detail::mask_whole_occurrences(masked, entry);
    }
  }
  return masked.find(wanted) != std::string::npos;
}

} // namespace detail
LIBTMUX_NAMESPACE_END
