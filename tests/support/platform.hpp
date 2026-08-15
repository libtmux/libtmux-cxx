#pragma once

// Platform facts a test may need, and how to say so when they are absent.
//
// The library runs on Linux and macOS. A few tests reach past the library to
// check something the kernel knows, and the interface for asking differs or is
// missing. Where it is missing the test skips and names what it wanted, which
// is the honest outcome: failing would report a platform difference as a bug,
// and quietly asking tmux instead would make the test tautological — several
// of these exist precisely to check tmux's own report against something else.

#include <gtest/gtest.h>

namespace libtmux::test {

// `/proc` — the Linux process filesystem. Tests use it to read what a process
// was really started with, rather than what tmux says it was.
#if defined(__linux__)
inline constexpr bool kHasProcfs = true;
#else
inline constexpr bool kHasProcfs = false;
#endif

} // namespace libtmux::test

#define LIBTMUX_SKIP_WITHOUT_PROCFS(wanted)                                            \
  do {                                                                                 \
    if constexpr (!::libtmux::test::kHasProcfs) {                                      \
      GTEST_SKIP() << "no /proc on this platform, so " << (wanted)                     \
                   << " cannot be read from the kernel; asking tmux instead would "    \
                      "be asking the thing under test";                                \
    }                                                                                  \
  } while (false)
