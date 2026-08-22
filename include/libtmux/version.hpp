#pragma once

// Parse and order tmux version strings.
//
// Suffixes follow bare releases; `next-` precedes its release and `master`
// follows all. Psmux 3.3.7 occupies `revision=7`, equal to tmux 3.3g.

#include "libtmux/abi.hpp"
#include "libtmux/expected.hpp"
#include <compare>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

LIBTMUX_NAMESPACE_BEGIN

enum class VersionError { missing_prefix, malformed };

[[nodiscard]] constexpr std::string_view to_string(VersionError error) noexcept {
  switch (error) {
  case VersionError::missing_prefix:
    return "the output does not begin with the tmux prefix";
  case VersionError::malformed:
    return "the version is not a number this can order";
  }
  return "unknown version error";
}

struct Version {
  std::uint32_t major{};
  std::uint32_t minor{};
  // 0 for a bare release, 1 for `a`, 2 for `b`, and so on. Psmux's numeric
  // third component occupies this existing slot as the corresponding number.
  std::uint32_t revision{};
  // A `next-` build precedes the release it leads to; `master` follows every
  // numbered release.
  bool prerelease{false};
  bool unbounded{false};

  [[nodiscard]] constexpr std::strong_ordering
  operator<=>(const Version& other) const noexcept {
    if (unbounded != other.unbounded) {
      return unbounded ? std::strong_ordering::greater : std::strong_ordering::less;
    }
    if (const auto order = major <=> other.major; order != 0) {
      return order;
    }
    if (const auto order = minor <=> other.minor; order != 0) {
      return order;
    }
    if (const auto order = revision <=> other.revision; order != 0) {
      return order;
    }
    if (prerelease != other.prerelease) {
      return prerelease ? std::strong_ordering::less : std::strong_ordering::greater;
    }
    return std::strong_ordering::equal;
  }
  [[nodiscard]] constexpr bool operator==(const Version&) const noexcept = default;
};

// Parse the first line of `tmux -V`, with or without its trailing newline.
[[nodiscard]] inline expected<Version, VersionError>
parse_version(std::string_view output) {
  if (const auto line_end = output.find_first_of("\r\n");
      line_end != std::string_view::npos) {
    output = output.substr(0, line_end);
  }
  constexpr std::string_view prefix = "tmux ";
  if (!output.starts_with(prefix)) {
    return unexpected(VersionError::missing_prefix);
  }
  output.remove_prefix(prefix.size());

  Version version;
  if (output == "master") {
    version.unbounded = true;
    return version;
  }
  if (output.starts_with("next-")) {
    version.prerelease = true;
    output.remove_prefix(5);
  }

  const std::size_t dot = output.find('.');
  if (dot == std::string_view::npos || dot == 0) {
    return unexpected(VersionError::malformed);
  }
  const auto digits = [](std::string_view text, std::uint32_t& out) -> bool {
    if (text.empty()) {
      return false;
    }
    out = 0;
    for (const char digit : text) {
      if (digit < '0' || digit > '9') {
        return false;
      }
      const auto value = static_cast<std::uint32_t>(digit - '0');
      if (out > (std::numeric_limits<std::uint32_t>::max() - value) / 10U) {
        return false;
      }
      out = out * 10U + value;
    }
    return true;
  };
  if (!digits(output.substr(0, dot), version.major)) {
    return unexpected(VersionError::malformed);
  }
  std::string_view rest = output.substr(dot + 1);
  std::size_t end = 0;
  while (end < rest.size() && rest[end] >= '0' && rest[end] <= '9') {
    ++end;
  }
  if (!digits(rest.substr(0, end), version.minor)) {
    return unexpected(VersionError::malformed);
  }
  const std::string_view suffix = rest.substr(end);
  if (suffix.empty()) {
    return version;
  }
  if (suffix.starts_with('.')) {
    if (!digits(suffix.substr(1), version.revision)) {
      return unexpected(VersionError::malformed);
    }
    return version;
  }
  if (suffix.size() != 1 || suffix[0] < 'a' || suffix[0] > 'z') {
    return unexpected(VersionError::malformed);
  }
  version.revision = static_cast<std::uint32_t>(suffix[0] - 'a') + 1;
  return version;
}

// The oldest release this library supports, matching the Python package.
inline constexpr Version kMinimumSupported{.major = 3, .minor = 2, .revision = 1};

[[nodiscard]] constexpr bool is_supported(const Version& version) noexcept {
  return version >= kMinimumSupported;
}

// This package's own version, not tmux's.
[[nodiscard]] std::string_view library_version() noexcept;

LIBTMUX_NAMESPACE_END
