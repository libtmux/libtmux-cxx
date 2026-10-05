// White-box: the private process runner, because the signal environment a
// child inherits is not observable through any public result.
#include "process.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <string>
#include <utility>
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

// The grace between SIGTERM and SIGKILL is for descendants that outlived the
// leader. A child leaving none must not pay it, and the leader's own status
// cannot say so: it is held unreaped until the group is killed. Speed is the
// claim, so it is timed, against a child that leaves a descendant ignoring
// SIGTERM: both runs start a shell, and what differs is the grace.
std::chrono::steady_clock::duration quickest_run(std::string script) {
  ProcessRequest request;
  request.executable = "/bin/sh";
  request.arguments = {Argument{"-c"}, Argument{std::move(script)}};
  request.timeout = std::chrono::seconds{10};
  auto quickest = std::chrono::steady_clock::duration::max();
  for (int attempt = 0; attempt < 5; ++attempt) {
    const auto started = std::chrono::steady_clock::now();
    auto reply = run_process(request, {}, libtmux::detail::DescendantPolicy::terminate);
    quickest = std::min(quickest, std::chrono::steady_clock::now() - started);
    EXPECT_TRUE(reply.has_value()) << reply.error().diagnostic;
  }
  return quickest;
}

TEST(SpawnSignals, TerminatingDescendantsWaitsOutNoGraceWithoutOne) {
  const auto without = quickest_run("exit 0");
  const auto with = quickest_run("(trap '' TERM; sleep 30) & exit 0");
  EXPECT_LT(without + std::chrono::milliseconds{50}, with);
}

} // namespace
