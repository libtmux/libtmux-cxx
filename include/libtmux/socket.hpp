#pragma once

// Build the connection arguments that select a tmux-compatible server.
//
// tmux selects a server either by socket name (`-L`) or by socket path
// (`-S`). A name is resolved under the socket directory, so it must be a
// single path component; a path is used verbatim. Either way the result must
// fit a UNIX domain socket address. Windows psmux supports names but has no
// `-S` transport, so socket paths are rejected there.

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

LIBTMUX_NAMESPACE_END
