#include "spawn_descriptors.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

#if defined(__linux__)
namespace {
std::atomic<bool> return_infinite_hard_limit{false};
std::atomic<int> reported_policy_error{0};

void record_policy_error(int error) {
  reported_policy_error.store(error, std::memory_order_release);
}
} // namespace

extern "C" int __real_getrlimit(int resource, rlimit* limit);

// Clang TSan reaches this interposer before its instrumentation is ready.
extern "C" int __wrap_getrlimit(int resource, rlimit* limit)
#if defined(__clang__)
#if __has_attribute(disable_sanitizer_instrumentation)
    __attribute__((disable_sanitizer_instrumentation))
#endif
#endif
{
  if (resource == RLIMIT_NOFILE &&
      return_infinite_hard_limit.load(std::memory_order_acquire)) {
    limit->rlim_cur = 256U;
    limit->rlim_max = RLIM_INFINITY;
    return 0;
  }
  return __real_getrlimit(resource, limit);
}
#endif

namespace {

class SpawnObjects final {
public:
  SpawnObjects() {
    actions_result = ::posix_spawn_file_actions_init(&actions);
    if (actions_result == 0) {
      attributes_result = ::posix_spawnattr_init(&attributes);
    }
  }

  ~SpawnObjects() {
    if (attributes_result == 0) {
      static_cast<void>(::posix_spawnattr_destroy(&attributes));
    }
    if (actions_result == 0) {
      static_cast<void>(::posix_spawn_file_actions_destroy(&actions));
    }
  }

  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attributes;
  int actions_result{-1};
  int attributes_result{-1};
};

#if defined(__linux__)
int retained_descriptor_is_safe_after_hard_limit_is_lowered() {
  constexpr int minimum_marker_descriptor = 512;
  std::array<int, 2> marker{-1, -1};
  if (::pipe(marker.data()) != 0) {
    return 1;
  }
  const auto high_marker = ::fcntl(marker[0], F_DUPFD, minimum_marker_descriptor);
  if (high_marker < minimum_marker_descriptor || ::close(marker[0]) != 0) {
    return 2;
  }
  marker[0] = high_marker;

  constexpr rlimit lowered_limit{256U, 256U};
  if (::setrlimit(RLIMIT_NOFILE, &lowered_limit) != 0) {
    return 3;
  }

  SpawnObjects spawn;
  if (spawn.actions_result != 0 || spawn.attributes_result != 0) {
    return 4;
  }
  libtmux::detail::force_numeric_spawn_descriptor_policy_for_test(nullptr);
  const auto policy = libtmux::detail::apply_spawn_descriptor_policy(
      spawn.actions, spawn.attributes, true);
  libtmux::detail::clear_spawn_descriptor_policy_test_override();
  if (!policy.has_value()) {
    return policy.error() == EBADF ? 0 : 5;
  }

  char sleep[] = "sleep";
  char duration[] = "30";
  char* arguments[]{sleep, duration, nullptr};
  pid_t child = -1;
  if (::posix_spawnp(&child, sleep, &spawn.actions, &spawn.attributes, arguments,
                     environ) != 0 ||
      ::close(marker[0]) != 0) {
    return 6;
  }
  pollfd watched{.fd = marker[1], .events = POLLOUT, .revents = 0};
  const auto polled = ::poll(&watched, 1, 1000);
  static_cast<void>(::kill(child, SIGKILL));
  int status = 0;
  static_cast<void>(::waitpid(child, &status, 0));
  static_cast<void>(::close(marker[1]));
  return polled == 1 && (watched.revents & POLLERR) != 0 ? 0 : 7;
}
#endif

TEST(SpawnDescriptors, PlatformPolicyClosesAnUnrelatedDescriptor) {
  std::array<int, 2> marker{-1, -1};
  ASSERT_EQ(::pipe(marker.data()), 0);

  SpawnObjects spawn;
  ASSERT_EQ(spawn.actions_result, 0);
  ASSERT_EQ(spawn.attributes_result, 0);
  const auto policy = libtmux::detail::apply_spawn_descriptor_policy(
      spawn.actions, spawn.attributes, true);
  ASSERT_TRUE(policy.has_value()) << policy.error();

  char sleep[] = "sleep";
  char duration[] = "30";
  char* arguments[]{sleep, duration, nullptr};
  pid_t child = -1;
  ASSERT_EQ(::posix_spawnp(&child, sleep, &spawn.actions, &spawn.attributes, arguments,
                           environ),
            0);
  const auto marker_closed = ::close(marker[0]);
  marker[0] = -1;

  pollfd watched{.fd = marker[1], .events = POLLOUT, .revents = 0};
  const auto polled = ::poll(&watched, 1, 1000);

  static_cast<void>(::kill(child, SIGKILL));
  int status = 0;
  static_cast<void>(::waitpid(child, &status, 0));
  static_cast<void>(::close(marker[1]));

  ASSERT_EQ(marker_closed, 0);
  ASSERT_EQ(polled, 1);
  EXPECT_NE(watched.revents & POLLERR, 0);
}

#if defined(__linux__)
TEST(SpawnDescriptors, ForcedNumericPolicyRefusesAnInfiniteHardLimit) {
  SpawnObjects spawn;
  ASSERT_EQ(spawn.actions_result, 0);
  ASSERT_EQ(spawn.attributes_result, 0);
  return_infinite_hard_limit.store(true, std::memory_order_release);
  reported_policy_error.store(0, std::memory_order_release);
  libtmux::detail::force_numeric_spawn_descriptor_policy_for_test(record_policy_error);

  const auto policy = libtmux::detail::apply_spawn_descriptor_policy(
      spawn.actions, spawn.attributes, false);
  libtmux::detail::clear_spawn_descriptor_policy_test_override();
  return_infinite_hard_limit.store(false, std::memory_order_release);

  ASSERT_FALSE(policy.has_value());
  EXPECT_EQ(policy.error(), EOVERFLOW);
  EXPECT_EQ(reported_policy_error.load(std::memory_order_acquire), EOVERFLOW);
}

TEST(SpawnDescriptors, NumericPolicyDoesNotLeakARetainedDescriptorAboveTheHardLimit) {
  rlimit original_limit{};
  ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &original_limit), 0);
  if (original_limit.rlim_cur <= 512U || original_limit.rlim_max <= 512U) {
    GTEST_SKIP() << "a descriptor limit above 512 is required";
  }

  const auto test_process = ::fork();
  ASSERT_GE(test_process, 0);
  if (test_process == 0) {
    ::_exit(retained_descriptor_is_safe_after_hard_limit_is_lowered());
  }
  int status = 0;
  ASSERT_EQ(::waitpid(test_process, &status, 0), test_process);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}
#endif

} // namespace
