#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace libtmux_psmux {

// psmux is the Windows implementation and tmux is everywhere else, so which
// rules apply is settled at compile time. The rules live in `rules` below and
// are never conditional: a naming rule nobody can compile is a naming rule
// nobody can test, and these have edge cases worth testing.
inline constexpr bool rules_apply =
#if defined(_WIN32)
    true;
#else
    false;
#endif

namespace rules {

[[nodiscard]] inline bool unsafe_command_argument(std::string_view value) noexcept {
  return value.find_first_of(";\r\n") != std::string_view::npos;
}

[[nodiscard]] inline bool missing_session(std::string_view diagnostic) noexcept {
  return diagnostic.find("psmux: no server running on session '") !=
         std::string_view::npos;
}

[[nodiscard]] inline char ascii_lower(char character) noexcept {
  return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a')
                                              : character;
}

[[nodiscard]] inline bool ascii_equal(std::string_view left,
                                      std::string_view right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (ascii_lower(left[index]) != ascii_lower(right[index])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] inline bool all_decimal(std::string_view value) noexcept {
  if (value.empty()) {
    return false;
  }
  for (const char character : value) {
    if (character < '0' || character > '9') {
      return false;
    }
  }
  return true;
}

[[nodiscard]] inline bool dos_device_name(std::string_view value) noexcept {
  const std::string_view base = value.substr(0, value.find('.'));
  if (ascii_equal(base, "con") || ascii_equal(base, "prn") ||
      ascii_equal(base, "aux") || ascii_equal(base, "nul")) {
    return true;
  }
  if (base.size() != 4U || base.back() < '1' || base.back() > '9') {
    return false;
  }
  return ascii_equal(base.substr(0, 3), "com") || ascii_equal(base.substr(0, 3), "lpt");
}

[[nodiscard]] inline std::optional<std::string>
invalid_registry_component(std::string_view value, std::string_view kind) {
  if (value.find("__") != std::string_view::npos) {
    return "a psmux " + std::string{kind} + " cannot contain '__'";
  }
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte < 0x20U ||
        std::string_view{R"(<>:"/\|?*;)"}.find(character) != std::string_view::npos) {
      return "a psmux " + std::string{kind} +
             " contains a character Windows cannot use in its registry files";
    }
  }
  if (value.ends_with('.') || value.ends_with(' ')) {
    return "a psmux " + std::string{kind} + " cannot end in a dot or space";
  }
  if (dos_device_name(value)) {
    return "a psmux " + std::string{kind} +
           " cannot use a reserved Windows device name";
  }
  return std::nullopt;
}

[[nodiscard]] inline std::optional<std::string>
invalid_session_name(std::string_view name) {
  if (auto invalid = invalid_registry_component(name, "session name");
      invalid.has_value()) {
    return invalid;
  }
  if (!name.empty() &&
      std::string_view{"-.=%@"}.find(name.front()) != std::string_view::npos) {
    return "a psmux session name cannot begin with a target metacharacter";
  }
  if (name.starts_with('$') && all_decimal(name.substr(1))) {
    return "a psmux session name cannot look like a session id";
  }
  // The last dot, not the first: psmux reads `session:window.pane`, so what
  // makes a name ambiguous is the suffix it ends with.
  const std::size_t dot = name.rfind('.');
  if (dot != std::string_view::npos && all_decimal(name.substr(dot + 1U))) {
    return "a psmux session name cannot end in a numeric target suffix";
  }
  return std::nullopt;
}

[[nodiscard]] inline std::optional<std::string>
invalid_socket_name(std::string_view name) {
  if (auto invalid = invalid_registry_component(name, "socket name");
      invalid.has_value()) {
    return invalid;
  }
  if (ascii_equal(name, "default")) {
    return "psmux cannot distinguish '-L default' from its default namespace";
  }
  return std::nullopt;
}

} // namespace rules

// What callers ask. Each answers "no objection" wherever psmux's rules do not
// apply, which is what every call site outside a platform branch relies on.
[[nodiscard]] inline bool unsafe_command_argument(std::string_view value) noexcept {
  return rules_apply && rules::unsafe_command_argument(value);
}

[[nodiscard]] inline bool missing_session(std::string_view diagnostic) noexcept {
  return rules_apply && rules::missing_session(diagnostic);
}

[[nodiscard]] inline std::optional<std::string>
invalid_session_name(std::string_view name) {
  return rules_apply ? rules::invalid_session_name(name) : std::nullopt;
}

[[nodiscard]] inline std::optional<std::string>
invalid_socket_name(std::string_view name) {
  return rules_apply ? rules::invalid_socket_name(name) : std::nullopt;
}

} // namespace libtmux_psmux
