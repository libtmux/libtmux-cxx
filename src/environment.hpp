#pragma once

#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace libtmux_env {

#if defined(_WIN32)
[[nodiscard]] inline char ascii_lower(char character) noexcept {
  return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a')
                                              : character;
}

[[nodiscard]] inline bool has_executable_extension(std::string_view value) noexcept {
  while (!value.empty()) {
    const auto separator = value.find(';');
    std::string_view token = value.substr(0, separator);
    while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) {
      token.remove_prefix(1);
    }
    while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) {
      token.remove_suffix(1);
    }
    constexpr std::string_view executable{".exe"};
    bool matches = token.size() == executable.size();
    for (std::size_t index = 0; matches && index < token.size(); ++index) {
      matches = ascii_lower(token[index]) == executable[index];
    }
    if (matches) {
      return true;
    }
    if (separator == std::string_view::npos) {
      break;
    }
    value.remove_prefix(separator + 1U);
  }
  return false;
}

[[nodiscard]] inline std::optional<std::string>
repaired_pathext(std::optional<std::string> inherited) {
  if (inherited.has_value() && has_executable_extension(*inherited)) {
    return std::nullopt;
  }
  std::string repaired{".COM;.EXE;.BAT;.CMD"};
  if (inherited.has_value() && !inherited->empty()) {
    repaired.push_back(';');
    repaired += *inherited;
  }
  return repaired;
}

#endif

[[nodiscard]] inline std::optional<std::string> value(std::string_view name) {
  if (name.empty() || name.find('\0') != std::string_view::npos) {
    return std::nullopt;
  }
#if defined(_WIN32)
  if (name.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return std::nullopt;
  }
  const int name_size = static_cast<int>(name.size());
  const int wide_name_size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                                   name.data(), name_size, nullptr, 0);
  if (wide_name_size == 0) {
    return std::nullopt;
  }
  std::wstring wide_name(static_cast<std::size_t>(wide_name_size), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), name_size,
                            wide_name.data(), wide_name_size) == 0) {
    return std::nullopt;
  }

  // Win32 environment updates need not update the CRT's getenv snapshot.
  const DWORD required = ::GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
  if (required <= 1U ||
      required > static_cast<DWORD>(std::numeric_limits<int>::max())) {
    return std::nullopt;
  }
  std::wstring wide(required, L'\0');
  const DWORD written =
      ::GetEnvironmentVariableW(wide_name.c_str(), wide.data(), required);
  if (written == 0U || written >= required) {
    return std::nullopt;
  }
  const auto wide_size = static_cast<int>(written);
  const int required_utf8 =
      ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), wide_size,
                            nullptr, 0, nullptr, nullptr);
  if (required_utf8 == 0) {
    return std::nullopt;
  }
  std::string result(static_cast<std::size_t>(required_utf8), '\0');
  if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), wide_size,
                            result.data(), required_utf8, nullptr, nullptr) == 0) {
    return std::nullopt;
  }
  return result;
#else
  const std::string owned_name{name};
  const char* const inherited = std::getenv(owned_name.c_str());
  if (inherited == nullptr || *inherited == '\0') {
    return std::nullopt;
  }
  return std::string{inherited};
#endif
}

#if defined(_WIN32)
[[nodiscard]] inline std::vector<std::pair<std::string, std::optional<std::string>>>
psmux_child_environment() {
  std::vector<std::pair<std::string, std::optional<std::string>>> environment{
      {"TMUX", std::nullopt},
      {"TMUX_PANE", std::nullopt},
      {"PSMUX_ACTIVE", std::nullopt},
      {"PSMUX_SESSION", std::nullopt},
      {"PSMUX_SESSION_NAME", std::nullopt},
      {"PSMUX_TARGET_FULL", std::nullopt},
      {"PSMUX_TARGET_SESSION", std::nullopt},
      {"PSMUX_REMOTE_ATTACH", std::nullopt},
      {"PSMUX_NO_WARM", "1"},
  };
  if (auto pathext = repaired_pathext(value("PATHEXT")); pathext.has_value()) {
    environment.emplace_back("PATHEXT", std::move(*pathext));
  }
  return environment;
}
#endif

} // namespace libtmux_env
