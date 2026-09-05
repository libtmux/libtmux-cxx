#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "libtmux/server.hpp"
#include "libtmux_consumers/mcp.hpp"

namespace libtmux::mcp::detail {

inline constexpr std::size_t kTargetCharacters = 512U;
inline constexpr std::size_t kSearchCharacters = 4096U;
inline constexpr std::size_t kRegexWorkUnits = 8U * 1024U * 1024U;
inline constexpr std::size_t kSearchMatchLimit = 1024U;

class BoundedRegex {
public:
  [[nodiscard]] static libtmux::expected<BoundedRegex, std::string>
  compile(std::string_view pattern);
  [[nodiscard]] libtmux::expected<bool, std::string>
  search(std::string_view text, std::size_t& remaining_work) const;

private:
  struct Atom {
    char value{};
    bool any{};
    bool repeated{};
  };

  explicit BoundedRegex(std::vector<Atom> atoms) : atoms_{std::move(atoms)} {}

  std::vector<Atom> atoms_;
};

[[nodiscard]] const std::string* argument(const Arguments& arguments,
                                          std::string_view name);
[[nodiscard]] ToolOutput output(StructuredValue::Object structured);
[[nodiscard]] ToolError tmux_error(const CommandFailure& error);
[[nodiscard]] StructuredValue session_value(const Session& session);
[[nodiscard]] StructuredValue window_value(const Window& window);
[[nodiscard]] StructuredValue pane_value(const Pane& pane);

} // namespace libtmux::mcp::detail
