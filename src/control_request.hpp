#pragma once

#include "libtmux/control.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

LIBTMUX_NAMESPACE_BEGIN
namespace detail {
namespace control_request {

inline bool names(std::string_view entered, std::string_view canonical,
                  std::string_view alias = {}) {
  return (!entered.empty() && canonical.starts_with(entered)) ||
         (!alias.empty() && entered == alias);
}

inline bool has_flag(const std::vector<std::string>& argv, char wanted,
                     std::string_view value_flags) {
  for (std::size_t index = 1U; index < argv.size(); ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--") {
      break;
    }
    if (argument.size() < 2U || argument.front() != '-' || argument[1] == '-') {
      break;
    }
    for (std::size_t offset = 1U; offset < argument.size(); ++offset) {
      const char flag = argument[offset];
      if (value_flags.find(flag) != std::string_view::npos) {
        if (offset + 1U == argument.size()) {
          ++index;
        }
        break;
      }
      if (flag == wanted) {
        return true;
      }
    }
  }
  return false;
}

inline bool is_no_insert_sentinel(const std::vector<std::string>& argv) {
  return argv.size() == 6U && argv[0] == "if-shell" && argv[1] == "-F" &&
         argv[2] == "-t" && argv[5] == "{";
}

inline std::optional<std::string> unsafe_command(const std::vector<std::string>& argv) {
  if (argv.empty()) {
    return std::nullopt;
  }
  const std::string_view name = argv.front();
  std::string_view canonical;
  if (names(name, "if-shell", "if")) {
    if (is_no_insert_sentinel(argv)) {
      return std::nullopt;
    }
    if (has_flag(argv, 'b', "t") && !has_flag(argv, 'F', "t")) {
      return std::nullopt;
    }
    canonical = "if-shell";
  } else if (names(name, "command-prompt")) {
    if (has_flag(argv, 'b', "IptT") || has_flag(argv, 'i', "IptT")) {
      return std::nullopt;
    }
    canonical = "command-prompt";
  } else if (names(name, "confirm-before", "confirm")) {
    if (has_flag(argv, 'b', "cpt")) {
      return std::nullopt;
    }
    canonical = "confirm-before";
  } else if (names(name, "display-panes", "displayp")) {
    if (has_flag(argv, 'b', "dt") || has_flag(argv, 'N', "dt")) {
      return std::nullopt;
    }
    canonical = "display-panes";
  } else if (names(name, "source-file", "source")) {
    if (has_flag(argv, 'n', "t")) {
      return std::nullopt;
    }
    canonical = "source-file";
  } else if (names(name, "run-shell", "run")) {
    if (!has_flag(argv, 'C', "dcst") || has_flag(argv, 'b', "dcst")) {
      return std::nullopt;
    }
    canonical = "run-shell -C";
  } else {
    return std::nullopt;
  }
  return std::string{canonical} +
         " can emit control reply blocks not counted by its request group";
}

inline std::optional<std::string> unsafe(const ControlRequest& request) {
  for (std::size_t index = 0U; index < request.group.size(); ++index) {
    auto diagnostic = unsafe_command(request.group[index].argv);
    if (diagnostic.has_value()) {
      return "control request operation " + std::to_string(index + 1U) + ": " +
             *std::move(diagnostic);
    }
  }
  return std::nullopt;
}

} // namespace control_request
} // namespace detail
LIBTMUX_NAMESPACE_END
