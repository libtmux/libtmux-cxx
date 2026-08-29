#pragma once

// Events tmux emits outside guarded control reply blocks.
//
// Most are protocol notifications. A wait-capable command may also print
// delayed output or errors there, without a request identifier. Raw bytes
// preserve that ambiguity; `parse` adds borrowed views only for known
// notification shapes.

#include "libtmux/abi.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

LIBTMUX_NAMESPACE_BEGIN

struct Notification {
  // Compatibility name for one outside-block control event. Unknown content
  // is not proof that tmux emitted a notification rather than delayed output.
  std::vector<std::byte> body;
};

// What a notification is, once its name and arguments have been read.
//
// `unknown` is not a failure. The body may be a notification added by a newer
// tmux or unguarded delayed command output. A kind and fields keep additions
// from breaking an exhaustive `std::visit` in caller code.
enum class NotificationKind : std::uint8_t {
  unknown,
  output,
  extended_output,
  paused,
  resumed,
  sessions_changed,
  session_changed,
  session_renamed,
  session_window_changed,
  client_detached,
  client_session_changed,
  window_add,
  window_close,
  window_renamed,
  window_pane_changed,
  unlinked_window_add,
  unlinked_window_close,
  unlinked_window_renamed,
  pane_mode_changed,
  paste_buffer_changed,
  paste_buffer_deleted,
  subscription_changed,
  config_error,
  exit,
  layout_change,
  message,
};

[[nodiscard]] std::string_view to_string(NotificationKind kind) noexcept;

// A notification's arguments, as views into the notification it was read from.
//
// tmux types its arguments by prefix — `$0` a session, `@1` a window, `%2` a
// pane — so each lands in the field it belongs to and the others stay empty.
// `payload` is the pane bytes of an output notification, already unescaped;
// it is empty for every other kind.
//
// Everything here borrows. The notification must outlive it, which is why
// there is no overload taking a temporary.
struct ParsedNotification {
  NotificationKind kind{NotificationKind::unknown};
  std::string_view name{};
  std::string_view session{};
  std::string_view window{};
  std::string_view pane{};
  std::string_view text{};
  std::span<const std::byte> payload{};
  // Milliseconds this output was behind when tmux wrote it. Only
  // `extended_output` carries one.
  std::optional<std::uint64_t> age{};
};

[[nodiscard]] ParsedNotification parse(const Notification& notification);
ParsedNotification parse(Notification&&) = delete;

namespace detail {
struct NotificationWatchState;
}

// One independent view of outside-block events emitted after opening.
//
// Taking from one watch never drains another watch or the Connection's legacy
// event queue. The retained log is shared and bounded; this watch's dropped
// count reports only events this watch actually missed.
//
// A watch retains the stream state. It can drain already-buffered events after
// its Connection is moved or destroyed; waits then wake as a closed stream.
class NotificationWatch final {
public:
  NotificationWatch() noexcept;
  ~NotificationWatch() noexcept;
  NotificationWatch(NotificationWatch&&) noexcept;
  NotificationWatch& operator=(NotificationWatch&&) noexcept;
  NotificationWatch(const NotificationWatch&) = delete;
  NotificationWatch& operator=(const NotificationWatch&) = delete;

  // Everything this watch has not taken yet, returned without waiting.
  [[nodiscard]] std::vector<Notification> take_notifications();
  // Wait for this watch to have an event, for the stream to close, or for the
  // deadline. An empty result means one of the latter two happened.
  [[nodiscard]] std::vector<Notification>
  wait_for_notifications(std::chrono::steady_clock::time_point deadline);
  // Readable when this watch has something to take or its stream has closed.
  // Do not read it; taking all available events clears this watch's byte while
  // the stream is open, and closure remains readable. `-1` means the pipe
  // could not be created or the watch is empty/moved from.
  [[nodiscard]] int notification_fd() const noexcept;
  // Events evicted before this watch took them, excluding events consumed by
  // other cursors and events emitted before this watch opened.
  [[nodiscard]] std::size_t dropped_notifications() const noexcept;

private:
  friend class Connection;
  explicit NotificationWatch(
      std::unique_ptr<detail::NotificationWatchState> state) noexcept;
  std::unique_ptr<detail::NotificationWatchState> state_;
};

LIBTMUX_NAMESPACE_END
