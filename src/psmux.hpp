#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace libtmux_psmux {

[[nodiscard]] inline bool unsafe_command_argument(std::string_view value) noexcept {
#if defined(_WIN32)
  return value.find_first_of(";\r\n") != std::string_view::npos;
#else
  static_cast<void>(value);
  return false;
#endif
}

[[nodiscard]] inline bool missing_session(std::string_view diagnostic) noexcept {
#if defined(_WIN32)
  return diagnostic.find("psmux: no server running on session '") !=
         std::string_view::npos;
#else
  static_cast<void>(diagnostic);
  return false;
#endif
}

#if defined(_WIN32)
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
#endif

[[nodiscard]] inline std::optional<std::string>
invalid_session_name(std::string_view name) {
#if defined(_WIN32)
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
  const std::size_t dot = name.find('.');
  if (dot != std::string_view::npos && all_decimal(name.substr(dot + 1U))) {
    return "a psmux session name cannot end in a numeric target suffix";
  }
#else
  static_cast<void>(name);
#endif
  return std::nullopt;
}

[[nodiscard]] inline std::optional<std::string>
invalid_socket_name(std::string_view name) {
#if defined(_WIN32)
  if (auto invalid = invalid_registry_component(name, "socket name");
      invalid.has_value()) {
    return invalid;
  }
  if (ascii_equal(name, "default")) {
    return "psmux cannot distinguish '-L default' from its default namespace";
  }
#else
  static_cast<void>(name);
#endif
  return std::nullopt;
}

} // namespace libtmux_psmux
