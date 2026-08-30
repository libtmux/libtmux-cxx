// White-box: the private process runner, because the signal environment a
// child inherits is not observable through any public result.
#include "process.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <string>
#include <variant>

#include <pthread.h>

namespace {

using libtmux::detail::Argument;
using libtmux::detail::ProcessRequest;
using libtmux::detail::run_process;
using libtmux::detail::Signaled;

// Signals itself and then reports having survived. A shell that inherits
// SIGTERM blocked or ignored cannot be killed by it, so the two outcomes are
// distinguishable without timing.
ProcessRequest self_terminating_shell() {
  ProcessRequest request;
  request.executable = "/bin/sh";
  request.arguments = {Argument{"-c"}, Argument{"kill -TERM $$; echo survived"}};
  request.timeout = std::chrono::seconds{10};
  return request;
}

TEST(SpawnSignals, ChildDoesNotInheritABlockedSignalMask) {
  sigset_t blocked;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGTERM);
  sigset_t previous;
  ASSERT_EQ(::pthread_sigmask(SIG_BLOCK, &blocked, &previous), 0);
  auto reply = run_process(self_terminating_shell());
  ASSERT_EQ(::pthread_sigmask(SIG_SETMASK, &previous, nullptr), 0);

  ASSERT_TRUE(reply.has_value()) << reply.error().diagnostic;
  EXPECT_TRUE(std::holds_alternative<Signaled>(reply->termination));
}

TEST(SpawnSignals, ChildDoesNotInheritAnIgnoredDisposition) {
  struct sigaction ignored {};
  ignored.sa_handler = SIG_IGN;
  sigemptyset(&ignored.sa_mask);
  struct sigaction previous {};
  ASSERT_EQ(::sigaction(SIGTERM, &ignored, &previous), 0);
  auto reply = run_process(self_terminating_shell());
  ASSERT_EQ(::sigaction(SIGTERM, &previous, nullptr), 0);

  ASSERT_TRUE(reply.has_value()) << reply.error().diagnostic;
  EXPECT_TRUE(std::holds_alternative<Signaled>(reply->termination));
}

} // namespace
