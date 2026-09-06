#pragma once

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace libtmux::mcp::detail {

// The environment, as the MCP reads it.
//
// A set-but-empty value is not the same as an unset one here: `LIBTMUX_TOOLSETS=`
// selects no toolset at all, and collapsing the two would hand that caller the
// default surface instead. `std::getenv` is standard C++; MSVC deprecates it
// anyway and `/WX` makes the nag fatal, and since these variables are written
// through `_putenv_s` the CRT's own snapshot is the one that has to be read.
[[nodiscard]] inline std::optional<std::string>
environment_value(std::string_view name) {
  const std::string owned{name};
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
  const char* const found = std::getenv(owned.c_str());
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
  if (found == nullptr) {
    return std::nullopt;
  }
  return std::string{found};
}

} // namespace libtmux::mcp::detail
