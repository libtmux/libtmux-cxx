#pragma once

// Parse and order tmux version strings.
//
// Suffixes follow bare releases; `next-` precedes its release, a release
// candidate sits between the two. Psmux 3.3.7 occupies `revision=7`, equal
// to tmux 3.3g.

#include "libtmux/abi.hpp"
#include "libtmux/expected.hpp"
#include <compare>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

LIBTMUX_NAMESPACE_BEGIN

/// Why a tmux version string could not be read.
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

/// A tmux version, ordered so a feature gate can compare against a literal.
///
/// A `next-` build and a release candidate order before the release they lead
/// to, so a gate written against `3.8` does not accept `3.8-rc2`. A behaviour
/// that candidates already have is gated at the earliest build measured to
/// have it, as `{.major = 3, .minor = 8, .candidate = 2}` for JSON layouts.
struct Version {
  std::uint32_t major{};
  std::uint32_t minor{};
  /// 0 for a bare release, 1 for `a`, 2 for `b`, and so on. Psmux's numeric
  /// third component occupies this existing slot as the corresponding number.
  std::uint32_t revision{};
  /// A `next-` build precedes the release it leads to.
  bool prerelease{false};
  /// Never set by `parse_version`. A harness that cannot read tmux's version
  /// sets it to mean "newest", so version-gated tests run rather than skip.
  bool unbounded{false};
  /// 0 for a release, 1 for `-rc` and N for `-rcN`: a candidate orders after a
  /// `next-` build and before its release.
  std::uint16_t candidate{0};

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
    // A release is the greatest of its candidates.
    const auto rank = [](std::uint16_t value) noexcept {
      return value == 0U ? std::numeric_limits<std::uint16_t>::max() : value;
    };
    return rank(candidate) <=> rank(other.candidate);
  }
  [[nodiscard]] constexpr bool operator==(const Version&) const noexcept = default;
};

/// Parse the first line of `tmux -V`, with or without its trailing newline.
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

  // `master`, `X.Y-master` and `X.Y-dev` are not forms tmux reports, and a
  // build that cannot say which release it leads to cannot be ordered, so they
  // are errors rather than versions that follow every release.
  Version version;
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
  // A `next-` build leads to a release and carries no patch letter or
  // candidate of its own.
  if (version.prerelease) {
    return unexpected(VersionError::malformed);
  }
  // A release candidate ranks below the release it names: `-rc`, or `-rc2`
  // for a later candidate of the same release.
  if (suffix.starts_with("-rc")) {
    std::uint32_t candidate = 1;
    if (suffix.size() == 3 || (digits(suffix.substr(3), candidate) && candidate != 0U &&
                               candidate < std::numeric_limits<std::uint16_t>::max())) {
      version.candidate = static_cast<std::uint16_t>(candidate);
      return version;
    }
    return unexpected(VersionError::malformed);
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

/// The oldest release this library supports: 3.3a. Patch letters count, so
/// 3.3 is below it, and `next-3.3a` and `3.3a-rc` order before it.
inline constexpr Version kMinimumSupported{.major = 3, .minor = 3, .revision = 1};

/// The floor as the messages name it; `parse_version` reads it back as
/// `kMinimumSupported`.
inline constexpr std::string_view kMinimumSupportedText{"3.3a"};

[[nodiscard]] constexpr bool is_supported(const Version& version) noexcept {
  return version >= kMinimumSupported;
}

/// This package's own version, not tmux's.
[[nodiscard]] std::string_view library_version() noexcept;

LIBTMUX_NAMESPACE_END

/// The same version, available to the preprocessor.
///
/// `library_version()` answers what was linked; these answer what was compiled
/// against, which is the question a consumer has to ask before using something
/// that may not exist yet:
///
///   `#if LIBTMUX_VERSION_MAJOR > 0 || LIBTMUX_VERSION_MINOR >= 2`
///
/// Written here rather than generated into the build directory, so that
/// `include/libtmux/` stays self-contained for anyone reading or vendoring it
/// without CMake. A test compares `LIBTMUX_VERSION_STRING` against
/// `library_version()`, which the build takes from the `VERSION` file, so the
/// two cannot drift apart unnoticed.
#define LIBTMUX_VERSION_MAJOR 0
#define LIBTMUX_VERSION_MINOR 1
#define LIBTMUX_VERSION_PATCH 0
#define LIBTMUX_VERSION_STRING "0.1.0-alpha.11"
