#pragma once

// Build the connection arguments that select a tmux-compatible server.
//
// tmux selects a server either by socket name (`-L`) or by socket path
// (`-S`). A name is resolved under the socket directory, so it must be a
// single path component; a path is used verbatim. Either way the result must
// fit a UNIX domain socket address. Windows psmux supports names but has no
// `-S` transport, so socket paths are rejected there.
//
// A caller that selects neither leaves the choice to the environment:
// `socket_from_environment()` reads `LIBTMUX_SOCKET_PATH` and `LIBTMUX_SOCKET`,
// and with neither set nothing is selected, so tmux picks the server itself.

#include "libtmux/abi.hpp"
#include "libtmux/expected.hpp"
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <sys/un.h>
#endif

LIBTMUX_NAMESPACE_BEGIN

// The address this platform can hold, less its terminator.
//
// Taken from the header rather than written down: it is 107 on Linux and 103
// on macOS, and the hardcoded Linux number let a 104-byte path pass validation
// on a platform that would then refuse it at connect time, with a message
// naming neither the limit nor the path — which is the failure this check
// exists to replace.
#if defined(_WIN32)
inline constexpr std::size_t kSocketPathLimit = 0U;
#else
inline constexpr std::size_t kSocketPathLimit = sizeof(sockaddr_un::sun_path) - 1U;
#endif

/// Why a socket name or path was rejected before any server was contacted.
///
/// Length is the one that surprises: a unix socket path is bounded by the
/// platform, not by tmux.
enum class SocketError {
  empty,
  name_has_separator,
  path_too_long,
  path_unsupported,
};

[[nodiscard]] constexpr std::string_view to_string(SocketError error) noexcept {
  switch (error) {
  case SocketError::empty:
    return "the socket selector is empty";
  case SocketError::name_has_separator:
    return "a socket name cannot contain a path separator";
  case SocketError::path_unsupported:
    return "psmux does not support socket paths on Windows; use a socket name";
  case SocketError::path_too_long:
    return "the socket path does not fit a unix domain address";
  }
  return "unknown socket error";
}

/// `-L name`: a single component resolved under the socket directory.
[[nodiscard]] inline expected<std::vector<std::string>, SocketError>
socket_name_arguments(std::string_view name) {
  if (name.empty()) {
    return unexpected(SocketError::empty);
  }
  if (name.find('/') != std::string_view::npos
#if defined(_WIN32)
      || name.find('\\') != std::string_view::npos
#endif
  ) {
    return unexpected(SocketError::name_has_separator);
  }
  return std::vector<std::string>{"-L", std::string{name}};
}

/// `-S path`: used verbatim, so the address limit applies to it directly.
[[nodiscard]] inline expected<std::vector<std::string>, SocketError>
socket_path_arguments(std::string_view path) {
  if (path.empty()) {
    return unexpected(SocketError::empty);
  }
#if defined(_WIN32)
  return unexpected(SocketError::path_unsupported);
#else
  if (path.size() > kSocketPathLimit) {
    return unexpected(SocketError::path_too_long);
  }
  return std::vector<std::string>{"-S", std::string{path}};
#endif
}

/// What chose a socket when the caller named none.
enum class SocketSource {
  none,          ///< Neither variable is set: tmux picks the server.
  path_variable, ///< `LIBTMUX_SOCKET_PATH`, tmux `-S`.
  name_variable, ///< `LIBTMUX_SOCKET`, tmux `-L`.
};

/// The variable a source names, for a diagnostic that must not quote its value.
[[nodiscard]] constexpr std::string_view to_string(SocketSource source) noexcept {
  switch (source) {
  case SocketSource::none:
    return "no socket variable";
  case SocketSource::path_variable:
    return "LIBTMUX_SOCKET_PATH";
  case SocketSource::name_variable:
    return "LIBTMUX_SOCKET";
  }
  return "unknown socket source";
}

/// The socket the environment selects, or none.
struct EnvironmentSocket {
  SocketSource source{SocketSource::none};
  /// The variable's value, as given: not trimmed and not checked.
  std::string value;

  /// `-S path` for the path variable, `-L name` for the name variable, and
  /// nothing at all when no variable is set. A value `socket_path_arguments()` or
  /// `socket_name_arguments()` rejects is rejected here.
  [[nodiscard]] expected<std::vector<std::string>, SocketError> arguments() const {
    switch (source) {
    case SocketSource::path_variable:
      return socket_path_arguments(value);
    case SocketSource::name_variable:
      return socket_name_arguments(value);
    case SocketSource::none:
      break;
    }
    return std::vector<std::string>{};
  }
};

/// Reads `LIBTMUX_SOCKET_PATH`, then `LIBTMUX_SOCKET`.
///
/// The path wins when both are set, as tmux's `-S` overrides `-L`. A variable
/// that is empty or only whitespace counts as unset, each one alone, so an
/// empty path leaves a set name in force. Any other value is returned as it
/// was given. `TMUX` is not read: tmux follows it by itself when no socket is
/// passed.
[[nodiscard]] EnvironmentSocket socket_from_environment();

LIBTMUX_NAMESPACE_END
