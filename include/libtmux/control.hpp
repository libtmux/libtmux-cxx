#pragma once

// Decode tmux's control protocol.
//
// A control-mode stream interleaves command reply blocks with asynchronous
// notifications. This parser turns bytes into those events and nothing else:
// no threads, no process, no executor. Feeding it is the caller's job, which
// is what lets the same decoder serve a synchronous read loop today and an
// async executor later without either appearing in this header.
//
// Framing preserves bytes. A line inside a block that looks like a
// notification stays block body, because control-mode framing does not make it
// independently attributable, and block bodies are never converted to UTF-8.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "libtmux/abi.hpp"
#include "libtmux/delivery.hpp"
#include "libtmux/expected.hpp"

LIBTMUX_NAMESPACE_BEGIN

// Why a control operation stopped. The stream is terminal after a protocol
// failure: a caller cannot resynchronise it, only start a new one. `delivery`
// says whether the affected request was untouched, fully written, answered,
// or left indeterminate.
struct ProtocolError {
  std::string message;
  DeliveryStatus delivery{DeliveryStatus::indeterminate};
};

enum class ControlTerminal : std::uint8_t { end, error };

// How much of one reply a decoder holds, and how long a single line may grow
// before the stream is called broken.
//
// A subprocess ends and gives its memory back; a connection does not, so the
// bound has to be in the decoder rather than in whatever reads it afterwards.
// The reply bound is the subprocess transport's capture limit, so the same
// call costs the same memory over either transport. The line bound has no
// subprocess equivalent: it is the point past which an unterminated line is
// evidence of a broken stream rather than a large answer.
inline constexpr std::size_t kDefaultRetainedReplyBytes = 1024U * 1024U;
inline constexpr std::size_t kDefaultLineBytes = 1024U * 1024U;

struct ControlBlock {
  std::uint64_t sequence;
  std::uint64_t command_number;
  ControlTerminal terminal;
  std::vector<std::byte> begin_metadata;
  std::vector<std::byte> terminal_metadata;
  std::vector<std::byte> body;
  // `body` holds the first `retained_reply_bytes` and stopped; `body_bytes` is
  // how many there were. Set rather than reported as an error because framing
  // is the parser's job and judging the answer is the caller's: the rest of the
  // reply is still drained, so the next command's reply is still attributable.
  bool body_truncated{false};
  std::size_t body_bytes{0};
};

struct Notification {
  std::vector<std::byte> body;
};

using Event = std::variant<ControlBlock, Notification>;

// What a notification is, once its name and arguments have been read.
//
// `unknown` is not a failure. tmux adds notification names over time, so a
// name this build does not know may be from a newer tmux, and the body is
// still there to read. A kind and fields keep such additions from breaking an
// exhaustive `std::visit` in caller code.
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

class Parser final {
public:
  Parser() = default;
  // Zero means unbounded, which only a test that owns both ends should ask
  // for.
  Parser(std::size_t retained_reply_bytes, std::size_t line_bytes) noexcept
      : retained_reply_bytes_{retained_reply_bytes}, line_bytes_{line_bytes} {}

  expected<std::vector<Event>, ProtocolError> feed(std::span<const std::byte> bytes);
  expected<void, ProtocolError> finish();

private:
  std::vector<std::byte> pending_;
  std::optional<ControlBlock> block_;
  std::optional<ProtocolError> failure_;
  std::size_t retained_reply_bytes_{kDefaultRetainedReplyBytes};
  std::size_t line_bytes_{kDefaultLineBytes};
  bool finished_{false};
};

struct ControlCommand {
  std::vector<std::string> argv;
};

struct ControlRequest {
  std::vector<ControlCommand> group;
};

struct ControlRequestResult {
  // Every synchronous reply block tmux emitted for this request, in wire
  // order. tmux does not put a request or operation ID on its guards, so a
  // command alias or inserted command may make this differ from `group`.
  std::vector<ControlBlock> blocks;
  std::optional<ProtocolError> connection_error;
};

struct ConnectionOptions {
  std::filesystem::path tmux_binary{"tmux"};
  std::filesystem::path socket_path{};
  std::string session_name{};
  std::chrono::milliseconds startup_timeout{2000};
  std::chrono::milliseconds shutdown_timeout{2000};
  // Passed to the decoder. Raise the first to hold a bigger capture; the
  // second bounds a line that never ends. A connection accepts zero
  // (unbounded) or at least 128 bytes, which leaves room for its private
  // request boundary.
  std::size_t retained_reply_bytes{kDefaultRetainedReplyBytes};
  std::size_t line_bytes{kDefaultLineBytes};

  // Deliver `%output` for every pane, as notifications.
  //
  // Off, so tmux is not asked to buffer pane output for a caller who never
  // reads it. It is fixed at connect time because tmux fixes it: a connection
  // started without output cannot be made to listen later, so this cannot be
  // a subscription. See `docs/design/pane-output-streaming.md`.
  bool pane_output{false};

  // Discard a pane's queued output once it is this far behind, and say so
  // with `%pause`.
  //
  // A data-loss policy rather than backpressure, and unset is a policy too:
  // tmux then buffers until a queued block is five minutes old and closes the
  // connection with `too far behind`. Set this and a slow reader survives
  // having lost output; leave it and a slow enough reader loses the
  // connection. Only meaningful with `pane_output`.
  //
  // `%pause` is the sole report that anything was dropped, and it names the
  // pane. A caller that sets this and ignores notifications has chosen to
  // lose output silently.
  std::optional<std::chrono::seconds> pause_after{};
};

class Connection;

// Everything tmux says, until the deadline, as one loop.
//
// Draining by hand is two nested loops and a break: ask for a batch, stop if
// it is empty, walk it, ask again. That shape was written six times across
// this repository's own tests and examples before this existed, which is the
// argument for it.
//
// An input range, single pass. A `ParsedNotification` views the notification
// it was read from, and this owns that notification only until the iterator
// advances — so copy what you need out of one before asking for the next.
class NotificationRange final {
public:
  class iterator final {
  public:
    using difference_type = std::ptrdiff_t;
    using value_type = ParsedNotification;
    using iterator_concept = std::input_iterator_tag;

    iterator() = default;
    explicit iterator(NotificationRange* range) : range_{range} { advance(); }

    [[nodiscard]] const ParsedNotification& operator*() const noexcept {
      return current_;
    }
    iterator& operator++() {
      advance();
      return *this;
    }
    void operator++(int) { advance(); }

    [[nodiscard]] bool operator==(std::default_sentinel_t) const noexcept {
      return range_ == nullptr;
    }

  private:
    void advance();

    NotificationRange* range_{nullptr};
    ParsedNotification current_{};
  };

  NotificationRange(Connection& connection,
                    std::chrono::steady_clock::time_point deadline) noexcept
      : connection_{&connection}, deadline_{deadline} {}

  [[nodiscard]] iterator begin() { return iterator{this}; }
  [[nodiscard]] std::default_sentinel_t end() const noexcept { return {}; }

private:
  friend class iterator;
  // The next notification, or nothing once the deadline has passed with none.
  [[nodiscard]] const Notification* next();

  Connection* connection_{nullptr};
  std::chrono::steady_clock::time_point deadline_{};
  std::vector<Notification> batch_{};
  std::size_t index_{0};
};

class Connection final {
public:
  static expected<Connection, ProtocolError> connect(ConnectionOptions options);

  ~Connection() noexcept;
  Connection(Connection&&) noexcept;
  Connection& operator=(Connection&&) noexcept;
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  // Completes at this request's private protocol boundary and preserves every
  // reply before it. It does not invent per-operation attribution that tmux
  // does not transmit.
  ControlRequestResult execute(ControlRequest request,
                               std::chrono::steady_clock::time_point deadline);
  // Everything tmux has said since the last call, returned at once.
  //
  // Taking drains: what comes back will not come back again. It says nothing
  // about what happens next, so an empty result does not mean the stream has
  // gone quiet, and a later one is new traffic rather than a repeat. Wait for
  // the next event with `wait_for_notifications` rather than polling for one.
  [[nodiscard]] std::vector<Notification> take_notifications();

  // The same, but waits for something to arrive.
  //
  // `take_notifications` returns immediately, so a caller reacting to tmux had
  // to call it in a loop and sleep between — which either wakes too often or
  // reacts too late, and picks that trade with no idea how long the next event
  // will take. This blocks until at least one notification is available, the
  // connection fails, or the deadline passes, and returns whatever it has.
  //
  // An empty result means the deadline passed or the stream ended; the two are
  // told apart by asking `execute` or `shutdown`, which report the failure.
  // Notifications already buffered are returned without waiting at all.
  [[nodiscard]] std::vector<Notification>
  wait_for_notifications(std::chrono::steady_clock::time_point deadline);
  // A descriptor that is readable exactly when a take would return something.
  //
  // For a caller who owns their own event loop. Without it, integrating means
  // a thread blocked in `wait_for_notifications`, a queue of their own, and a
  // self-pipe to wake the loop — which is this descriptor, rebuilt by hand on
  // top of the thread and queue this connection already has.
  //
  // Do not read from it: readability is the signal and the byte is this
  // connection's to consume. Drain with `take_notifications`, which clears it.
  // A broken stream makes it readable too, so a poller learns of the failure
  // rather than waiting for an answer that cannot come.
  //
  // Valid until the connection is destroyed or moved from; `-1` if the pipe
  // could not be created.
  [[nodiscard]] int notification_fd() const noexcept;

  // Stop or resume `%output` for one pane, on a connection that asked for it.
  //
  // The direction is not symmetrical, because tmux is not: a connection that
  // started without `pane_output` cannot be made to listen to anything, and
  // muting is the only per-pane control it offers. So this narrows what a
  // listening connection receives; it cannot widen a silent one.
  //
  // `resume` on a pane that tmux paused also clears the pause, and tmux moves
  // that pane's offset to the current end — so whatever was produced while it
  // was paused or muted is not delivered afterwards.
  expected<void, ProtocolError>
  set_pane_output(std::string_view pane, bool deliver,
                  std::chrono::steady_clock::time_point deadline);

  // Everything tmux says until the deadline, as one loop rather than two.
  //
  // Borrows this connection, which must outlive it.
  [[nodiscard]] NotificationRange
  events(std::chrono::steady_clock::time_point deadline);

  // How many notifications were discarded to keep the buffer bounded, which
  // is what distinguishes a quiet connection from one that outran its reader.
  [[nodiscard]] std::size_t dropped_notifications() const noexcept;
  [[nodiscard]] std::int64_t native_child_pid() const noexcept;
  expected<void, ProtocolError>
  shutdown(std::chrono::steady_clock::time_point deadline);

private:
  struct State;
  explicit Connection(std::unique_ptr<State> state) noexcept;
  std::unique_ptr<State> state_;
};

LIBTMUX_NAMESPACE_END
