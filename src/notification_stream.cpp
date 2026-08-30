#include "notification_stream.hpp"

#include <algorithm>
#include <array>
#include <fcntl.h>
#include <utility>

#include <unistd.h>

LIBTMUX_NAMESPACE_BEGIN
namespace detail {
namespace {

void close_descriptor(int& descriptor) noexcept {
  if (descriptor >= 0) {
    static_cast<void>(::close(descriptor));
    descriptor = -1;
  }
}

void make_wake_pipe(NotificationCursor& cursor) noexcept {
  std::array<int, 2> wake{-1, -1};
  if (::pipe(wake.data()) != 0) {
    return;
  }
  for (const int descriptor : wake) {
    const int flags = ::fcntl(descriptor, F_GETFL, 0);
    if (flags >= 0) {
      static_cast<void>(::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK));
    }
    const int descriptor_flags = ::fcntl(descriptor, F_GETFD, 0);
    if (descriptor_flags >= 0) {
      static_cast<void>(::fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC));
    }
  }
  cursor.wake_read = wake[0];
  cursor.wake_write = wake[1];
}

} // namespace

NotificationCursor::~NotificationCursor() noexcept {
  close_descriptor(wake_write);
  close_descriptor(wake_read);
}

std::shared_ptr<NotificationCursor> NotificationStream::subscribe(bool replay) {
  auto cursor = std::make_shared<NotificationCursor>();
  make_wake_pipe(*cursor);
  std::lock_guard lock{mutex_};
  cursor->next_sequence =
      replay && !entries_.empty() ? entries_.front().sequence : next_sequence_;
  if (closed_ || cursor->next_sequence < next_sequence_) {
    arm_locked(*cursor);
  }
  cursors_.push_back(cursor);
  return cursor;
}

void NotificationStream::unsubscribe(
    const std::shared_ptr<NotificationCursor>& cursor) noexcept {
  if (!cursor) {
    return;
  }
  std::lock_guard lock{mutex_};
  std::erase_if(cursors_, [&](const auto& held) {
    const auto live = held.lock();
    return !live || live == cursor;
  });
  prune_consumed_locked();
}

void NotificationStream::arm_locked(NotificationCursor& cursor) const noexcept {
  if (cursor.wake_armed || cursor.wake_write < 0) {
    return;
  }
  const char byte = 1;
  if (::write(cursor.wake_write, &byte, 1) == 1) {
    cursor.wake_armed = true;
  }
}

void NotificationStream::disarm_locked(NotificationCursor& cursor) const noexcept {
  if (!cursor.wake_armed || cursor.wake_read < 0) {
    return;
  }
  char byte = 0;
  if (::read(cursor.wake_read, &byte, 1) == 1) {
    cursor.wake_armed = false;
  }
}

void NotificationStream::sweep_cursors_locked() {
  std::erase_if(cursors_, [&](const auto& held) {
    const auto cursor = held.lock();
    if (!cursor) {
      return true;
    }
    if (closed_ || cursor->next_sequence < next_sequence_) {
      arm_locked(*cursor);
    }
    return false;
  });
}

void NotificationStream::prune_consumed_locked() {
  std::uint64_t first_needed = next_sequence_;
  std::erase_if(cursors_, [&](const auto& held) {
    const auto cursor = held.lock();
    if (!cursor) {
      return true;
    }
    first_needed = std::min(first_needed, cursor->next_sequence);
    return false;
  });
  while (!entries_.empty() && entries_.front().sequence < first_needed) {
    bytes_ -= entries_.front().notification.body.size();
    entries_.pop_front();
  }
}

void NotificationStream::push(Notification notification) {
  std::lock_guard lock{mutex_};
  const auto sequence = next_sequence_++;
  const auto arriving = notification.body.size();
  if (maximum_count_ != 0U && arriving <= maximum_bytes_) {
    while (!entries_.empty() &&
           (entries_.size() >= maximum_count_ || bytes_ > maximum_bytes_ - arriving)) {
      bytes_ -= entries_.front().notification.body.size();
      entries_.pop_front();
    }
    entries_.push_back(Entry{sequence, std::move(notification)});
    bytes_ += arriving;
  }
  sweep_cursors_locked();
  condition_.notify_all();
}

void NotificationStream::close() noexcept {
  std::lock_guard lock{mutex_};
  if (closed_) {
    return;
  }
  closed_ = true;
  sweep_cursors_locked();
  condition_.notify_all();
}

std::vector<Notification>
NotificationStream::collect_locked(NotificationCursor& cursor) {
  std::vector<Notification> held;
  held.reserve(entries_.size());
  auto expected = cursor.next_sequence;
  for (const auto& entry : entries_) {
    if (entry.sequence < expected) {
      continue;
    }
    cursor.dropped += static_cast<std::size_t>(entry.sequence - expected);
    held.push_back(entry.notification);
    expected = entry.sequence + 1U;
  }
  cursor.dropped += static_cast<std::size_t>(next_sequence_ - expected);
  cursor.next_sequence = next_sequence_;
  if (!closed_) {
    disarm_locked(cursor);
  }
  prune_consumed_locked();
  return held;
}

std::vector<Notification>
NotificationStream::take(const std::shared_ptr<NotificationCursor>& cursor) {
  if (!cursor) {
    return {};
  }
  std::lock_guard lock{mutex_};
  return collect_locked(*cursor);
}

std::vector<Notification>
NotificationStream::wait(const std::shared_ptr<NotificationCursor>& cursor,
                         std::chrono::steady_clock::time_point deadline) {
  if (!cursor) {
    return {};
  }
  std::unique_lock lock{mutex_};
  static_cast<void>(condition_.wait_until(lock, deadline, [&] {
    return cursor->next_sequence < next_sequence_ || closed_;
  }));
  return collect_locked(*cursor);
}

int NotificationStream::notification_fd(
    const std::shared_ptr<NotificationCursor>& cursor) const noexcept {
  return cursor ? cursor->wake_read : -1;
}

std::size_t NotificationStream::pending_drops_locked(
    const NotificationCursor& cursor) const noexcept {
  auto expected = cursor.next_sequence;
  std::size_t dropped = cursor.dropped;
  for (const auto& entry : entries_) {
    if (entry.sequence < expected) {
      continue;
    }
    dropped += static_cast<std::size_t>(entry.sequence - expected);
    expected = entry.sequence + 1U;
  }
  dropped += static_cast<std::size_t>(next_sequence_ - expected);
  return dropped;
}

std::size_t NotificationStream::dropped(
    const std::shared_ptr<NotificationCursor>& cursor) const noexcept {
  if (!cursor) {
    return 0U;
  }
  std::lock_guard lock{mutex_};
  return pending_drops_locked(*cursor);
}

NotificationWatchState::~NotificationWatchState() noexcept {
  if (stream) {
    stream->unsubscribe(cursor);
  }
}

} // namespace detail

NotificationWatch::NotificationWatch() noexcept = default;
NotificationWatch::NotificationWatch(
    std::unique_ptr<detail::NotificationWatchState> state) noexcept
    : state_{std::move(state)} {}

NotificationWatch::~NotificationWatch() noexcept = default;
NotificationWatch::NotificationWatch(NotificationWatch&&) noexcept = default;
NotificationWatch& NotificationWatch::operator=(NotificationWatch&&) noexcept = default;

std::vector<Notification> NotificationWatch::take_notifications() {
  return state_ ? state_->stream->take(state_->cursor) : std::vector<Notification>{};
}

std::vector<Notification> NotificationWatch::wait_for_notifications(
    std::chrono::steady_clock::time_point deadline) {
  return state_ ? state_->stream->wait(state_->cursor, deadline)
                : std::vector<Notification>{};
}

int NotificationWatch::notification_fd() const noexcept {
  return state_ ? state_->stream->notification_fd(state_->cursor) : -1;
}

std::size_t NotificationWatch::dropped_notifications() const noexcept {
  return state_ ? state_->stream->dropped(state_->cursor) : 0U;
}

LIBTMUX_NAMESPACE_END
