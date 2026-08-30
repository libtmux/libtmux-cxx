#pragma once

// The signal environment this library gives the children it starts.
//
// A blocked mask and an ignored disposition both survive exec, and tmux
// restores the mask it inherited rather than an empty one. A signal blocked
// here stays blocked in the server tmux starts, which then cannot be
// terminated by it, and in every pane command that sets no mask of its own.

#include "libtmux/abi.hpp"

#include <cerrno>
#include <csignal>
#include <spawn.h>

LIBTMUX_NAMESPACE_BEGIN
namespace detail {

// Adds default dispositions and an empty mask to `extra_flags`. Returns zero,
// or an errno value naming the call that refused.
[[nodiscard]] inline int apply_clean_signal_attributes(posix_spawnattr_t& attributes,
                                                       short extra_flags) noexcept {
  sigset_t defaulted;
  sigset_t unblocked;
  if (sigfillset(&defaulted) != 0 || sigdelset(&defaulted, SIGKILL) != 0 ||
      sigdelset(&defaulted, SIGSTOP) != 0 || sigemptyset(&unblocked) != 0) {
    return errno;
  }
  const auto flags =
      static_cast<short>(extra_flags | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
  if (const auto result = ::posix_spawnattr_setflags(&attributes, flags); result != 0) {
    return result;
  }
  if (const auto result = ::posix_spawnattr_setsigdefault(&attributes, &defaulted);
      result != 0) {
    return result;
  }
  return ::posix_spawnattr_setsigmask(&attributes, &unblocked);
}

} // namespace detail
LIBTMUX_NAMESPACE_END
