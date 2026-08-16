#pragma once

// Build `send-keys` arguments.
//
// tmux does not report an unknown key name: `send-keys NoSuchKey` succeeds
// silently, so a typo is invisible at the call site and shows up later as
// input that never arrived. Key names are therefore validated here, where the
// caller can still be told.
//
// Literal text takes `-l`, under which tmux interprets nothing — not key
// names, not formats. Choosing between the two is the caller's decision and is
// never inferred from the text.

#include "libtmux/abi.hpp"
#include "libtmux/expected.hpp"
#include <array>
#include <string>
#include <string_view>
#include <vector>

LIBTMUX_NAMESPACE_BEGIN

enum class KeyError { empty, unknown_name };

[[nodiscard]] constexpr std::string_view to_string(KeyError error) noexcept {
  switch (error) {
  case KeyError::empty:
    return "there are no keys to send";
  case KeyError::unknown_name:
    return "tmux does not know that key name";
  }
  return "unknown key error";
}

// Named keys tmux accepts, taken from the table in its own key-string.c at
// the oldest supported release. Function keys are generated rather than
// listed, and a single character is handled separately.
inline constexpr std::array kNamedKeys{
    std::string_view{"BSpace"},   std::string_view{"BTab"},
    std::string_view{"DC"},       std::string_view{"Delete"},
    std::string_view{"Down"},     std::string_view{"End"},
    std::string_view{"Enter"},    std::string_view{"Escape"},
    std::string_view{"Home"},     std::string_view{"IC"},
    std::string_view{"Insert"},   std::string_view{"KP*"},
    std::string_view{"KP+"},      std::string_view{"KP-"},
    std::string_view{"KP."},      std::string_view{"KP/"},
    std::string_view{"KP0"},      std::string_view{"KP1"},
    std::string_view{"KP2"},      std::string_view{"KP3"},
    std::string_view{"KP4"},      std::string_view{"KP5"},
    std::string_view{"KP6"},      std::string_view{"KP7"},
    std::string_view{"KP8"},      std::string_view{"KP9"},
    std::string_view{"KPEnter"},  std::string_view{"Left"},
    std::string_view{"NPage"},    std::string_view{"PPage"},
    std::string_view{"PageDown"}, std::string_view{"PageUp"},
    std::string_view{"PgDn"},     std::string_view{"PgUp"},
    std::string_view{"Right"},    std::string_view{"Space"},
    std::string_view{"Tab"},      std::string_view{"Up"},
};

[[nodiscard]] inline bool is_function_key(std::string_view name) noexcept {
  if (name.size() < 2 || name.front() != 'F') {
    return false;
  }
  const std::string_view digits = name.substr(1);
  // tmux names at most F12, so anything longer than two digits is already out
  // of range and must not be accumulated: doing so overflows before the range
  // check can reject it.
  if (digits.empty() || digits.size() > 2) {
    return false;
  }
  unsigned value = 0;
  for (const char digit : digits) {
    if (digit < '0' || digit > '9') {
      return false;
    }
    value = value * 10 + static_cast<unsigned>(digit - '0');
  }
  return value >= 1 && value <= 12;
}

// Accept a key with any number of C-, M-, or S- modifiers.
[[nodiscard]] inline bool is_key_name(std::string_view key) noexcept {
  while (key.size() > 2 && key[1] == '-' &&
         (key[0] == 'C' || key[0] == 'M' || key[0] == 'S')) {
    key.remove_prefix(2);
  }
  if (key.empty()) {
    return false;
  }
  if (key.size() == 1) {
    // tmux takes any printable character as itself, and rejects a control
    // byte: `send-keys` with one delivers nothing.
    return static_cast<unsigned char>(key.front()) >= 0x20U;
  }
  if (is_function_key(key)) {
    return true;
  }
  for (const std::string_view named : kNamedKeys) {
    if (key == named) {
      return true;
    }
  }
  return false;
}

// Send text exactly as written: the flag, the end of flags, and the text.
//
// `--` is part of the fragment rather than something a caller appends, because
// text beginning with a dash is read as another `send-keys` option without it
// and that is not a mistake worth making twice. It was made twice: this
// returned the flag and the text alone, `Pane::send_text` inserted the
// separator afterwards, and `Chain::send_text` did not — so the same text
// through the two spellings reached tmux as two different commands.
[[nodiscard]] inline expected<std::vector<std::string>, KeyError>
literal_arguments(std::string_view text) {
  if (text.empty()) {
    return unexpected(KeyError::empty);
  }
  return std::vector<std::string>{"-l", "--", std::string{text}};
}

LIBTMUX_NAMESPACE_END
