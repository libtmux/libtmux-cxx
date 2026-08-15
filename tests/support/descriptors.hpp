#pragma once

// How many descriptors this process has open.
//
// Used by tests that prove a leak did not happen: take a count, do the thing,
// take it again. `/proc/self/fd` is the obvious way to ask and is Linux-only,
// so there is a second answer for everywhere else — probing each descriptor
// number with `fcntl`, which costs a syscall per slot but needs no procfs.
//
// The two are interchangeable for the only thing tests do with them: compare
// one count against another taken the same way.

#include <algorithm>
#include <cstddef>

#if defined(__linux__)
#include <filesystem>
#include <iterator>
#else
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace libtmux::test {

[[nodiscard]] inline std::size_t open_descriptor_count() {
#if defined(__linux__)
  return static_cast<std::size_t>(
      std::distance(std::filesystem::directory_iterator{"/proc/self/fd"},
                    std::filesystem::directory_iterator{}));
#else
  // The soft limit rather than `_SC_OPEN_MAX`, which on macOS can be large
  // enough to make the scan the slowest thing in the suite. Capped as well,
  // because a raised limit is a property of the machine and not of the test.
  rlimit limit{};
  const long long ceiling =
      ::getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY
          ? static_cast<long long>(limit.rlim_cur)
          : 1024;
  const long long scanned = std::min<long long>(ceiling, 4096);

  std::size_t open = 0;
  for (long long descriptor = 0; descriptor < scanned; ++descriptor) {
    if (::fcntl(static_cast<int>(descriptor), F_GETFD) != -1) {
      ++open;
    }
  }
  return open;
#endif
}

} // namespace libtmux::test
