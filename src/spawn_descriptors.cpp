#include "spawn_descriptors.hpp"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <limits>

#include <dirent.h>
#include <sys/resource.h>

#if defined(__GLIBC__)
#include <features.h>
#endif

LIBTMUX_NAMESPACE_BEGIN
namespace detail {
namespace {

#if defined(LIBTMUX_SPAWN_DESCRIPTOR_TEST_SEAM)
std::atomic<bool> test_force_numeric{false};
std::atomic<SpawnDescriptorPolicyTestHook> test_after_policy{nullptr};
#endif

expected<void, int> apply_numeric_policy(posix_spawn_file_actions_t& actions,
                                         std::uintmax_t ceiling) noexcept {
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
  return {};
}

#if defined(__linux__)
expected<std::uintmax_t, int> finite_hard_ceiling() noexcept {
  rlimit descriptor_limit{};
  if (::getrlimit(RLIMIT_NOFILE, &descriptor_limit) != 0) {
    return unexpected(errno);
  }
  if (descriptor_limit.rlim_max == RLIM_INFINITY) {
    return unexpected(EOVERFLOW);
  }
  const auto ceiling = static_cast<std::uintmax_t>(descriptor_limit.rlim_max);
  if (ceiling > static_cast<std::uintmax_t>(std::numeric_limits<int>::max())) {
    return unexpected(EOVERFLOW);
  }
  return ceiling;
}

expected<std::uintmax_t, int> live_descriptor_ceiling() noexcept {
  auto* directory = ::opendir("/proc/self/fd");
  if (directory == nullptr) {
    return unexpected(errno);
  }

  std::uintmax_t highest = 2U;
  int scan_error = 0;
  for (;;) {
    errno = 0;
    const auto* entry = ::readdir(directory);
    if (entry == nullptr) {
      scan_error = errno;
      break;
    }

    std::uintmax_t descriptor = 0U;
    bool numeric = entry->d_name[0] != '\0';
    for (const char* character = entry->d_name; *character != '\0'; ++character) {
      if (*character < '0' || *character > '9') {
        numeric = false;
        break;
      }
      const auto digit = static_cast<std::uintmax_t>(*character - '0');
      if (descriptor > (std::numeric_limits<std::uintmax_t>::max() - digit) / 10U) {
        scan_error = EOVERFLOW;
        numeric = false;
        break;
      }
      descriptor = descriptor * 10U + digit;
    }
    if (scan_error != 0) {
      break;
    }
    if (numeric && descriptor > highest) {
      highest = descriptor;
    }
  }

  const auto close_result = ::closedir(directory);
  if (scan_error != 0) {
    return unexpected(scan_error);
  }
  if (close_result != 0) {
    return unexpected(errno);
  }
  if (highest >= static_cast<std::uintmax_t>(std::numeric_limits<int>::max())) {
    return unexpected(EOVERFLOW);
  }
  return highest + 1U;
}

expected<void, int>
apply_linux_numeric_policy(posix_spawn_file_actions_t& actions) noexcept {
  const auto hard_before_scan = finite_hard_ceiling();
  if (!hard_before_scan.has_value()) {
    return unexpected(hard_before_scan.error());
  }
  const auto live_ceiling = live_descriptor_ceiling();
  if (!live_ceiling.has_value()) {
    return unexpected(live_ceiling.error());
  }
  const auto hard_after_scan = finite_hard_ceiling();
  if (!hard_after_scan.has_value()) {
    return unexpected(hard_after_scan.error());
  }
  const auto allocation_ceiling =
      *hard_before_scan > *hard_after_scan ? *hard_before_scan : *hard_after_scan;
  const auto exhaustive_ceiling =
      allocation_ceiling > *live_ceiling ? allocation_ceiling : *live_ceiling;
  return apply_numeric_policy(actions, exhaustive_ceiling);
}
#endif

} // namespace

expected<void, int>
apply_spawn_descriptor_policy(posix_spawn_file_actions_t& actions,
                              posix_spawnattr_t& attributes,
                              bool inherit_standard_streams) noexcept {
#if defined(LIBTMUX_SPAWN_DESCRIPTOR_TEST_SEAM)
  if (test_force_numeric.load(std::memory_order_acquire)) {
    const auto result = apply_linux_numeric_policy(actions);
    if (const auto after_policy = test_after_policy.load(std::memory_order_acquire);
        after_policy != nullptr) {
      after_policy(result.has_value() ? 0 : result.error());
    }
    return result;
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
  return apply_linux_numeric_policy(actions);
#else
  static_cast<void>(actions);
  static_cast<void>(attributes);
  static_cast<void>(inherit_standard_streams);
  return unexpected(ENOTSUP);
#endif
}

#if defined(LIBTMUX_SPAWN_DESCRIPTOR_TEST_SEAM)
void force_numeric_spawn_descriptor_policy_for_test(
    SpawnDescriptorPolicyTestHook after_policy) noexcept {
  test_after_policy.store(after_policy, std::memory_order_release);
  test_force_numeric.store(true, std::memory_order_release);
}

void clear_spawn_descriptor_policy_test_override() noexcept {
  test_force_numeric.store(false, std::memory_order_release);
  test_after_policy.store(nullptr, std::memory_order_release);
}
#endif

} // namespace detail
LIBTMUX_NAMESPACE_END
