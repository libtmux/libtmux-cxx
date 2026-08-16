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
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "libtmux/abi.hpp"
#include "libtmux/expected.hpp"

LIBTMUX_NAMESPACE_BEGIN

// Why a decode stopped. The stream is terminal for its connection: a caller
// cannot resynchronise a control stream, only start a new one.
struct ProtocolError {
  std::string message;
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

enum class Attribution : std::uint8_t { exact, skipped, unknown };

struct ControlCommand {
  std::vector<std::string> argv;
};

struct ControlRequest {
  std::vector<ControlCommand> group;
};

struct ControlOperationResult {
  Attribution attribution{Attribution::unknown};
  std::optional<ControlBlock> block;
};

struct ControlRequestResult {
  std::vector<ControlOperationResult> operations;
  std::optional<ProtocolError> connection_error;
};

struct ConnectionOptions {
  std::filesystem::path tmux_binary{"tmux"};
  std::filesystem::path socket_path;
  std::string session_name;
  std::chrono::milliseconds startup_timeout{2000};
  std::chrono::milliseconds shutdown_timeout{2000};
  // Passed to the decoder. Raise the first to hold a bigger capture; the
  // second bounds a line that never ends and wants raising only if tmux grows
  // a longer one.
  std::size_t retained_reply_bytes{kDefaultRetainedReplyBytes};
  std::size_t line_bytes{kDefaultLineBytes};
};

class Connection final {
public:
  static expected<Connection, ProtocolError> connect(ConnectionOptions options);

  ~Connection() noexcept;
  Connection(Connection&&) noexcept;
  Connection& operator=(Connection&&) noexcept;
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  ControlRequestResult execute(ControlRequest request,
                               std::chrono::steady_clock::time_point deadline);
  // Everything tmux has said since the last call, and how many were dropped
  // to keep the buffer bounded.
  [[nodiscard]] std::vector<Notification> take_notifications();
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
