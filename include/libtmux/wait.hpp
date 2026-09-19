#pragma once

// Wait for a pane to say something.
//
// This is the first thing a supervising program needs and the easiest to get
// wrong, which is why it belongs here rather than in each consumer. The hard
// parts are not the loop: they are telling output apart from a command still
// sitting on the prompt (`output_confirms` in `capture.hpp`), preferring the
// control stream's `%output` over re-reading the screen, and falling back to
// capture when no connection can be opened.

#include "libtmux/abi.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

LIBTMUX_NAMESPACE_BEGIN

// Which path answered the wait. A caller that reports on how it learned
// something needs this; a caller that does not can ignore it.
enum class WaitPath : std::uint8_t {
  // Already on screen when the wait began, and credited to the pane.
  capture_at_entry,
  // Found by the capture taken right after the control connection opened,
  // which closes the window between the entry capture and the first
  // notification.
  capture_after_control_connect,
  // The budget expired before a connection could be opened.
  capture_before_control,
  // A `%output` notification for this pane prompted the capture that matched.
  control_output,
  // No connection could be opened, so the screen was re-read on a timer.
  capture_polling,
};

struct WaitResult {
  // `text` contains `wanted` as something the pane produced.
  bool matched{};
  // `wanted` was already on screen before the wait began. True whether or not
  // the wait went on to credit it to the pane, so a caller can tell a fresh
  // line apart from one that was always there.
  bool matched_at_entry{};
  bool timed_out{};
  // `wanted` is on screen, but every occurrence of it is text the caller
  // itself sent — so the pane has not produced it, and this is not a match.
  // Distinct from a silent timeout, where `wanted` never appeared at all.
  bool present_unconfirmed{};
  std::chrono::milliseconds elapsed{};
  WaitPath path{};
  // The last capture taken, matched or not.
  std::string text{};
};

struct WaitOptions {
  std::chrono::milliseconds timeout{std::chrono::seconds{10}};
  // What the calling program itself typed into this pane and has not had
  // confirmed. Every occurrence is erased before the wanted text is looked
  // for, so a command's own echo is never credited to the pane as its output.
  // See `output_confirms`.
  std::vector<std::string> sent{};
  // How much text this wait will search before giving up. A pane that prints
  // faster than the search can read it would otherwise spin until the
  // deadline; this reports instead. Counted across every capture, not per
  // capture.
  std::size_t match_budget{8U * 1024U * 1024U};
  // How long the fallback waits between captures, and the longest the event
  // path blocks before checking the deadline and cancellation.
  std::chrono::milliseconds poll_interval{50};
  // Whether the caller has given up. Polled rather than a `std::stop_token`
  // because the loop already wakes on `poll_interval` and most callers have a
  // flag rather than a token; a token adapts in one line —
  // `[stop] { return stop.stop_requested(); }` — while the reverse would cost
  // the caller a thread. A cancelled wait answers `FailureKind::cancelled`.
  std::function<bool()> cancelled{};
  // Called at most once per second while the wait runs, with how long it has
  // been waiting and how it is currently waiting. For a caller forwarding
  // progress to somebody else; the wait does not need it.
  std::function<void(std::chrono::milliseconds elapsed, WaitPath path)> on_progress{};
  // Called with the control client's process id when the wait opens one, and
  // expected to answer a guard that the wait holds until it closes that
  // connection. A caller that reports on attached clients needs this, or its
  // own observation reads as somebody else being attached.
  std::function<std::shared_ptr<void>(std::int64_t pid)> observe_control_client{};
};

LIBTMUX_NAMESPACE_END
