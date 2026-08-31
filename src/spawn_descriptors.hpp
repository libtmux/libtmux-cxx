#pragma once

#include "libtmux/abi.hpp"
#include "libtmux/expected.hpp"

#include <spawn.h>

LIBTMUX_NAMESPACE_BEGIN
namespace detail {

using SpawnDescriptorPolicyTestHook = void (*)(int);

// Keeps descriptors supplied by spawn actions and, when requested, inherited
// standard streams. Native policies close every other descriptor. The Linux
// numeric fallback assumes no concurrent privileged hard-limit increase;
// ordinary concurrent allocation and soft-limit changes remain covered.
[[nodiscard]] expected<void, int>
apply_spawn_descriptor_policy(posix_spawn_file_actions_t& actions,
                              posix_spawnattr_t& attributes,
                              bool inherit_standard_streams) noexcept;

#if defined(LIBTMUX_SPAWN_DESCRIPTOR_TEST_SEAM)
void force_numeric_spawn_descriptor_policy_for_test(
    SpawnDescriptorPolicyTestHook after_policy) noexcept;
void clear_spawn_descriptor_policy_test_override() noexcept;
#endif

} // namespace detail
LIBTMUX_NAMESPACE_END
