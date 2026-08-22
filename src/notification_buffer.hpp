#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "libtmux/control.hpp"

LIBTMUX_NAMESPACE_BEGIN
namespace detail {

inline constexpr std::size_t kMaximumNotifications = 4096U;

// A caller that never drains must not grow the connection for its lifetime;
// retain the newest notifications and account for the oldest ones discarded.
inline void retain_notification(std::vector<Notification>& notifications,
                                std::size_t& dropped, Notification notification) {
  if (notifications.size() >= kMaximumNotifications) {
    notifications.erase(notifications.begin());
    ++dropped;
  }
  notifications.push_back(std::move(notification));
}

} // namespace detail
LIBTMUX_NAMESPACE_END
