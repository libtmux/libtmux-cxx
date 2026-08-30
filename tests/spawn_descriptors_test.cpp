#include "spawn_descriptors.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>

#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

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
TEST(SpawnDescriptors, ForcedNumericPolicyRefusesANonRepresentableCeiling) {
  SpawnObjects spawn;
  ASSERT_EQ(spawn.actions_result, 0);
  ASSERT_EQ(spawn.attributes_result, 0);
  libtmux::detail::set_spawn_descriptor_policy_test_override(
      static_cast<std::uintmax_t>(std::numeric_limits<int>::max()) + 1U, nullptr);

  const auto policy = libtmux::detail::apply_spawn_descriptor_policy(
      spawn.actions, spawn.attributes, false);
  libtmux::detail::set_spawn_descriptor_policy_test_override(0U, nullptr);

  ASSERT_FALSE(policy.has_value());
  EXPECT_EQ(policy.error(), EOVERFLOW);
}
#endif

} // namespace
