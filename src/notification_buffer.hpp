#pragma once

#include <cstddef>
#include <deque>
#include <utility>
#include <vector>

#include "libtmux/control.hpp"

LIBTMUX_NAMESPACE_BEGIN
namespace detail {

inline constexpr std::size_t kMaximumNotifications = 4096U;
inline constexpr std::size_t kMaximumNotificationBytes = 16U * 1024U * 1024U;

// Retain the newest complete notifications within both bounds. A deque makes
// eviction constant-time; a vector erased at the front copied every survivor.
class NotificationBuffer final {
public:
  explicit NotificationBuffer(
      std::size_t maximum_count = kMaximumNotifications,
      std::size_t maximum_bytes = kMaximumNotificationBytes) noexcept
      : maximum_count_{maximum_count}, maximum_bytes_{maximum_bytes} {}

  void push(Notification notification) {
    const auto arriving = notification.body.size();
    if (maximum_count_ == 0U || arriving > maximum_bytes_) {
      ++dropped_;
      return;
    }
    while (!entries_.empty() &&
           (entries_.size() >= maximum_count_ || bytes_ > maximum_bytes_ - arriving)) {
      bytes_ -= entries_.front().body.size();
      entries_.pop_front();
      ++dropped_;
    }
    entries_.push_back(std::move(notification));
    bytes_ += arriving;
  }

  [[nodiscard]] std::vector<Notification> take() {
    std::vector<Notification> held;
    held.reserve(entries_.size());
    for (auto& notification : entries_) {
      held.push_back(std::move(notification));
    }
    entries_.clear();
    bytes_ = 0U;
    return held;
  }

  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] std::size_t dropped() const noexcept { return dropped_; }

private:
  std::deque<Notification> entries_;
  std::size_t maximum_count_;
  std::size_t maximum_bytes_;
  std::size_t bytes_{0U};
  std::size_t dropped_{0U};
};

} // namespace detail
LIBTMUX_NAMESPACE_END
