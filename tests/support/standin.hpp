// Makes a stand-in for tmux from the `tmux_standin` helper. See
// support/tmux_standin.cpp for the modes and their settings.

#pragma once

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

#ifndef LIBTMUX_TMUX_STANDIN_PATH
#error "the test target must define LIBTMUX_TMUX_STANDIN_PATH"
#endif

namespace libtmux::test {

using StandInSetting = std::pair<std::string_view, std::string_view>;

// Copy the helper to `path` and write its settings beside it. Returns false
// when either step fails.
[[nodiscard]] inline bool make_standin(const std::filesystem::path& path,
                                       std::initializer_list<StandInSetting> settings) {
  std::error_code failure;
  std::filesystem::copy_file(LIBTMUX_TMUX_STANDIN_PATH, path,
                             std::filesystem::copy_options::overwrite_existing,
                             failure);
  if (failure) {
    return false;
  }
  std::ofstream file{path.string() + ".standin", std::ios::trunc};
  for (const auto& [key, value] : settings) {
    file << key << '\t';
    for (const char character : value) {
      if (character == '\n') {
        file << "\\n";
      } else if (character == '\t') {
        file << "\\t";
      } else if (character == '\\') {
        file << "\\\\";
      } else {
        file << character;
      }
    }
    file << '\n';
  }
  file.close();
  return static_cast<bool>(file);
}

} // namespace libtmux::test
