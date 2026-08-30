#include "spawn_descriptors.hpp"

#include <atomic>
#include <cerrno>
#include <limits>

#include <sys/resource.h>

#if defined(__GLIBC__)
#include <features.h>
#endif

LIBTMUX_NAMESPACE_BEGIN
namespace detail {
namespace {

#if defined(LIBTMUX_SPAWN_DESCRIPTOR_TEST_SEAM)
std::atomic<std::uintmax_t> test_numeric_ceiling{0U};
std::atomic<SpawnDescriptorPolicyTestHook> test_after_actions{nullptr};
#endif

expected<void, int>
apply_numeric_policy(posix_spawn_file_actions_t& actions, std::uintmax_t ceiling,
                     SpawnDescriptorPolicyTestHook after_actions) noexcept {
  if (ceiling <= 3U) {
    return unexpected(EINVAL);
  }
  if (ceiling > static_cast<std::uintmax_t>(std::numeric_limits<int>::max())) {
    return unexpected(EOVERFLOW);
  }
  for (std::uintmax_t descriptor = 3U; descriptor < ceiling; ++descriptor) {
    const auto result =
        ::posix_spawn_file_actions_addclose(&actions, static_cast<int>(descriptor));
    if (result != 0) {
      return unexpected(result);
    }
  }
  if (after_actions != nullptr) {
    after_actions();
  }
  return {};
}

} // namespace

expected<void, int>
apply_spawn_descriptor_policy(posix_spawn_file_actions_t& actions,
                              posix_spawnattr_t& attributes,
                              bool inherit_standard_streams) noexcept {
#if defined(LIBTMUX_SPAWN_DESCRIPTOR_TEST_SEAM)
  const auto forced_ceiling = test_numeric_ceiling.load(std::memory_order_acquire);
  if (forced_ceiling != 0U) {
    return apply_numeric_policy(actions, forced_ceiling,
                                test_after_actions.load(std::memory_order_acquire));
  }
#endif

#if defined(__GLIBC__)
#if __GLIBC_PREREQ(2, 34)
  static_cast<void>(attributes);
  static_cast<void>(inherit_standard_streams);
  if (const auto result = ::posix_spawn_file_actions_addclosefrom_np(&actions, 3);
      result != 0) {
    return unexpected(result);
  }
  return {};
#endif
#endif
#if defined(__APPLE__)
  short flags = 0;
  if (const auto result = ::posix_spawnattr_getflags(&attributes, &flags);
      result != 0) {
    return unexpected(result);
  }
  flags = static_cast<short>(flags | POSIX_SPAWN_CLOEXEC_DEFAULT);
  if (const auto result = ::posix_spawnattr_setflags(&attributes, flags); result != 0) {
    return unexpected(result);
  }
  if (inherit_standard_streams) {
    for (int descriptor = 0; descriptor <= 2; ++descriptor) {
      if (const auto result =
              ::posix_spawn_file_actions_addinherit_np(&actions, descriptor);
          result != 0) {
        return unexpected(result);
      }
    }
  }
  return {};
#elif defined(__linux__)
  static_cast<void>(attributes);
  static_cast<void>(inherit_standard_streams);
  rlimit descriptor_limit{};
  if (::getrlimit(RLIMIT_NOFILE, &descriptor_limit) != 0) {
    return unexpected(errno);
  }
  if (descriptor_limit.rlim_cur == RLIM_INFINITY) {
    return unexpected(EOVERFLOW);
  }
  return apply_numeric_policy(
      actions, static_cast<std::uintmax_t>(descriptor_limit.rlim_cur), nullptr);
#else
  static_cast<void>(actions);
  static_cast<void>(attributes);
  static_cast<void>(inherit_standard_streams);
  return unexpected(ENOTSUP);
#endif
}

#if defined(LIBTMUX_SPAWN_DESCRIPTOR_TEST_SEAM)
void set_spawn_descriptor_policy_test_override(
    std::uintmax_t numeric_ceiling,
    SpawnDescriptorPolicyTestHook after_actions) noexcept {
  if (numeric_ceiling == 0U) {
    test_numeric_ceiling.store(0U, std::memory_order_release);
    test_after_actions.store(nullptr, std::memory_order_release);
    return;
  }
  test_after_actions.store(after_actions, std::memory_order_release);
  test_numeric_ceiling.store(numeric_ceiling, std::memory_order_release);
}
#endif

} // namespace detail
LIBTMUX_NAMESPACE_END
