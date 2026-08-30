#pragma once

#include "libtmux/notification.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

LIBTMUX_NAMESPACE_BEGIN
namespace detail {

inline constexpr std::size_t kMaximumNotifications = 4096U;
inline constexpr std::size_t kMaximumNotificationBytes = 16U * 1024U * 1024U;

struct NotificationCursor final {
  ~NotificationCursor() noexcept;

  std::uint64_t next_sequence{0U};
  std::size_t dropped{0U};
  int wake_read{-1};
  int wake_write{-1};
  bool wake_armed{false};
};

// One bounded retained log with independent cursors. Entries are stored once;
// a slow watch loses old entries rather than multiplying the memory bound by
// the number of consumers.
class NotificationStream final {
public:
  explicit NotificationStream(
      std::size_t maximum_count = kMaximumNotifications,
      std::size_t maximum_bytes = kMaximumNotificationBytes) noexcept
      : maximum_count_{maximum_count}, maximum_bytes_{maximum_bytes} {}

  [[nodiscard]] std::shared_ptr<NotificationCursor> subscribe(bool replay);
  void unsubscribe(const std::shared_ptr<NotificationCursor>& cursor) noexcept;
  void push(Notification notification);
  void close() noexcept;

  [[nodiscard]] std::vector<Notification>
  take(const std::shared_ptr<NotificationCursor>& cursor);
  [[nodiscard]] std::vector<Notification>
  wait(const std::shared_ptr<NotificationCursor>& cursor,
       std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] int
  notification_fd(const std::shared_ptr<NotificationCursor>& cursor) const noexcept;
  [[nodiscard]] std::size_t
  dropped(const std::shared_ptr<NotificationCursor>& cursor) const noexcept;

private:
  struct Entry {
    std::uint64_t sequence;
    Notification notification;
  };

  void arm_locked(NotificationCursor& cursor) const noexcept;
  void disarm_locked(NotificationCursor& cursor) const noexcept;
  void sweep_cursors_locked();
  void prune_consumed_locked();
  [[nodiscard]] std::vector<Notification> collect_locked(NotificationCursor& cursor);
  [[nodiscard]] std::size_t
  pending_drops_locked(const NotificationCursor& cursor) const noexcept;

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<Entry> entries_;
  std::vector<std::weak_ptr<NotificationCursor>> cursors_;
  std::size_t maximum_count_;
  std::size_t maximum_bytes_;
  std::size_t bytes_{0U};
  std::uint64_t next_sequence_{0U};
  bool closed_{false};
};

struct NotificationWatchState final {
  NotificationWatchState(std::shared_ptr<NotificationStream> stream,
                         std::shared_ptr<NotificationCursor> cursor) noexcept
      : stream{std::move(stream)}, cursor{std::move(cursor)} {}
  ~NotificationWatchState() noexcept;

  std::shared_ptr<NotificationStream> stream;
  std::shared_ptr<NotificationCursor> cursor;
};

} // namespace detail
LIBTMUX_NAMESPACE_END
