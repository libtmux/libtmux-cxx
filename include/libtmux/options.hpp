#pragma once

// Parse `show-options`-shaped output.
//
// tmux prints one option per line as `name value`, and an array option as
// `name[index] value`. The value may be quoted when it contains spaces, and an
// option with an empty value prints its name alone. Keeping all three shapes in
// one parser means callers never hand-split option output again.

#include "libtmux/abi.hpp"
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

LIBTMUX_NAMESPACE_BEGIN

struct OptionEntry {
  std::string name;
  /// Present only for array options, which tmux indexes sparsely: absent
  /// indexes are genuinely unset rather than empty.
  std::optional<std::size_t> index;
  std::string value;
  /// True when the value comes from a wider scope rather than being set here.
  /// `show-options -A` marks these with a trailing asterisk on the name.
  ///
  /// tmux accepts a user option whose name itself ends in an asterisk, and
  /// prints `@star* value` for one set here — which is the same shape as
  /// `status-position* bottom` for an inherited option. In a listing the two
  /// cannot be told apart, and the marker reading wins. Asking for one option
  /// by name is exact, because the name asked for settles it.
  bool inherited{};
};

/// Undo the quoting tmux applies when it prints a value.
///
/// tmux picks one of four forms, and a reader that knows only one corrupts the
/// rest: `''` for an empty value; double quotes when the value contains any of
/// ` #';${}`; single quotes when it contains a double quote; and otherwise no
/// quotes at all. Inside any of them the body is escaped the way `vis` does it
/// — `\\t`, `\\n`, `\\\\`, `\\ooo` for a byte with no printable form — and a
/// leading tilde is escaped whether or not anything else is.
///
/// This is the inverse of tmux's own `args_escape`, so a value read here and
/// written back is the value that was there.
[[nodiscard]] inline std::string unquote(std::string_view value) {
  if (value == "''") {
    return {};
  }
  if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                            (value.front() == '\'' && value.back() == '\''))) {
    value = value.substr(1, value.size() - 2);
  }

  std::string result;
  result.reserve(value.size());
  for (std::size_t position = 0; position < value.size(); ++position) {
    if (value[position] != '\\' || position + 1U == value.size()) {
      result.push_back(value[position]);
      continue;
    }
    const char escaped = value[position + 1U];
    ++position;
    switch (escaped) {
    case 'n':
      result.push_back('\n');
      break;
    case 't':
      result.push_back('\t');
      break;
    case 'r':
      result.push_back('\r');
      break;
    case 'a':
      result.push_back('\a');
      break;
    case 'b':
      result.push_back('\b');
      break;
    case 'f':
      result.push_back('\f');
      break;
    case 'v':
      result.push_back('\v');
      break;
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7': {
      // Up to three octal digits. tmux writes all three when the byte is
      // followed by something that would otherwise extend the number.
      unsigned byte = 0;
      std::size_t digits = 0;
      while (digits < 3U && position < value.size() && value[position] >= '0' &&
             value[position] <= '7') {
        byte = (byte * 8U) + static_cast<unsigned>(value[position] - '0');
        ++position;
        ++digits;
      }
      --position;
      result.push_back(static_cast<char>(byte & 0xFFU));
      break;
    }
    default:
      // A quote, a backslash, a tilde, a dollar: itself.
      result.push_back(escaped);
      break;
    }
  }
  return result;
}

/// Parse one line. Returns nullopt for a blank line so callers can feed raw
/// output straight in.
[[nodiscard]] inline std::optional<OptionEntry> parse_option(std::string_view line) {
  if (line.empty()) {
    return std::nullopt;
  }
  const std::size_t space = line.find(' ');
  std::string_view key = line.substr(0, space);
  const std::string_view value =
      space == std::string_view::npos ? std::string_view{} : line.substr(space + 1);

  OptionEntry entry;
  // The inheritance marker is part of the listing, not of the option name.
  // Stripping it here keeps a name comparison working whether or not the
  // caller asked for inherited options.
  if (key.ends_with('*')) {
    entry.inherited = true;
    key.remove_suffix(1);
  }
  if (key.ends_with(']')) {
    if (const std::size_t open = key.rfind('['); open != std::string_view::npos) {
      const std::string_view digits = key.substr(open + 1, key.size() - open - 2);
      std::size_t index = 0;
      bool numeric = !digits.empty();
      for (const char digit : digits) {
        if (digit < '0' || digit > '9') {
          numeric = false;
          break;
        }
        index = index * 10 + static_cast<std::size_t>(digit - '0');
      }
      if (numeric) {
        entry.index = index;
        key = key.substr(0, open);
      }
    }
  }
  entry.name = std::string{key};
  entry.value = unquote(value);
  return entry;
}

[[nodiscard]] inline std::vector<OptionEntry> parse_options(std::string_view output) {
  std::vector<OptionEntry> entries;
  std::size_t position = 0;
  while (position <= output.size()) {
    const std::size_t end = output.find('\n', position);
    const std::string_view line =
        output.substr(position, end == std::string_view::npos ? std::string_view::npos
                                                              : end - position);
    if (std::optional<OptionEntry> entry = parse_option(line)) {
      entries.push_back(*std::move(entry));
    }
    if (end == std::string_view::npos) {
      break;
    }
    position = end + 1;
  }
  return entries;
}

LIBTMUX_NAMESPACE_END
