#pragma once

#include <chrono>

#include <sys/types.h>

namespace libtmux::test {

// Waits for the owning wait call without consuming the child's status.
bool wait_until_reaped(pid_t pid, std::chrono::steady_clock::time_point deadline);

} // namespace libtmux::test
