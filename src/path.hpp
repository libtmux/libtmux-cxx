#pragma once

#include <filesystem>
#include <string>

namespace libtmux_path {

[[nodiscard]] inline std::string command_string(const std::filesystem::path& path) {
#if defined(_WIN32)
  const auto value = path.u8string();
  return {reinterpret_cast<const char*>(value.data()), value.size()};
#else
  return path.string();
#endif
}

} // namespace libtmux_path
