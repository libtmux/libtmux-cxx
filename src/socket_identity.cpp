#include "socket_identity.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#if !defined(_WIN32)
#include <unistd.h>
#endif

LIBTMUX_NAMESPACE_BEGIN
namespace detail {

#if defined(_WIN32)

std::optional<std::string>
resolved_socket_path(const std::vector<std::string>& selector) {
  if (selector.empty()) {
    return std::string{"psmux:default"};
  }
  if (selector.size() == 2U && selector.front() == "-L") {
    return std::string{"psmux:-L:"} + selector.back();
  }
  return std::nullopt;
}

#else

namespace {

// tmux compiles `TMUX_SOCK` as `"$TMUX_TMPDIR:" _PATH_TMP` and takes the first
// entry that resolves. An unset, empty or unresolvable `TMUX_TMPDIR` therefore
// falls through to `/tmp` rather than failing, which is why this asks the
// filesystem instead of trusting the variable.
std::optional<std::filesystem::path> socket_directory() {
  std::error_code failed;
  if (const char* const configured = std::getenv("TMUX_TMPDIR");
      configured != nullptr && *configured != '\0') {
    auto resolved = std::filesystem::canonical(configured, failed);
    if (!failed) {
      return resolved;
    }
  }
  auto fallback = std::filesystem::canonical("/tmp", failed);
  if (failed) {
    return std::nullopt;
  }
  return fallback;
}

} // namespace

std::optional<std::string>
resolved_socket_path(const std::vector<std::string>& selector) {
  const bool pair = selector.size() == 2U;
  if (pair && selector.front() == "-S") {
    // tmux uses a `-S` path verbatim, so the only question here is whether two
    // spellings name one socket — and they do when they resolve alike, because
    // the kernel resolves the address too. Weakly, because the socket need not
    // exist yet: a server is often addressed before it is started.
    std::error_code failed;
    auto resolved = std::filesystem::weakly_canonical(selector.back(), failed);
    if (failed) {
      return selector.back();
    }
    return resolved.string();
  }

  std::string label{"default"};
  if (pair && selector.front() == "-L") {
    label = selector.back();
  } else if (!selector.empty()) {
    return std::nullopt;
  }

  const auto directory = socket_directory();
  if (!directory.has_value()) {
    return std::nullopt;
  }
  return (*directory / ("tmux-" + std::to_string(::getuid())) / label).string();
}

#endif

} // namespace detail
LIBTMUX_NAMESPACE_END
