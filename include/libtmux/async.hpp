#pragma once

// Explicit bounded ownership for asynchronous Server commands.
// Results and global observations remain independent after admission.

#include "libtmux/abi.hpp"

#include "libtmux/command.hpp"
#include "libtmux/expected.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

LIBTMUX_NAMESPACE_BEGIN

class Server;

/// The maximum number of accepted commands retaining any lifecycle leg.
/// `start` rejects zero with `DeliveryStatus::not_started`.
struct CommandRuntimeConfig final {
  std::size_t capacity{256U};
};

/// A lock-consistent instant; values may change immediately after it is read.
/// Admission, refusal, and completion totals are monotonic.
struct CommandRuntimeSnapshot final {
  std::size_t capacity{};
  /// Accepted commands retaining transport, result, or observer work.
  std::size_t in_flight{};
  /// Result handles not yet waited, detached, or destroyed.
  std::size_t pending_results{};
  /// Observer records not yet claimed for dispatch or discarded, ready or not.
  std::size_t pending_observers{};
  /// Commands admitted since startup.
  std::uint64_t accepted{};
  /// Commands refused before admission since startup.
  std::uint64_t refused{};
  /// Accepted results whose observer record, when present, is also ready.
  std::uint64_t completed{};
  /// True while neither stop nor close has ended admission.
  bool accepting{};
};

/// The first successful `close` report; every later call returns this value.
/// Discharge result and observer work first when `safe_to_unload` must be true.
struct CommandRuntimeShutdown final {
  /// Result obligations outstanding when the first close succeeded.
  std::size_t pending_results{};
  /// Observer obligations outstanding when the first close succeeded.
  std::size_t pending_observers{};
  /// Whether every owned child and transport thread retired.
  bool transports_stopped{};
  /// True only when transports and pending work ended and no caller-side
  /// dispatch or discard, including callback-target teardown, remains active.
  /// Callers must prevent concurrent or later runtime entry before unloading.
  bool safe_to_unload{};
  /// The first runtime lifecycle failure; an operation may also carry it.
  std::optional<CommandFailure> failure;
};

/// Move-only owner of the transport threads, admission bound, and observations.
/// Runtime operations, including `try_submit`, may race; move and destruction
/// may not.
class CommandRuntime final {
public:
  /// Starts the bounded transport; startup failures are not-started values.
  static expected<CommandRuntime, CommandFailure>
  start(CommandRuntimeConfig config = {});
  CommandRuntime(CommandRuntime&&) noexcept;
  CommandRuntime& operator=(CommandRuntime&&) noexcept;
  CommandRuntime(const CommandRuntime&) = delete;
  CommandRuntime& operator=(const CommandRuntime&) = delete;
  // Stops and joins owned threads; pending observers are discarded, not
  // invoked.
  // Call `close` first when its terminal report matters.
  ~CommandRuntime();

  /// Stops admission and requests cancellation without waiting.
  void request_stop() noexcept;
  /// Joins every owned thread without invoking or discarding observers.
  /// The first successful report is cached; a throwing shutdown can be retried.
  [[nodiscard]] CommandRuntimeShutdown close();
  /// Reads one lock-consistent instant without waiting for work.
  [[nodiscard]] CommandRuntimeSnapshot snapshot() const noexcept;
  /// Runs one snapshot of ready observers on this thread and returns its count.
  /// A callback exception releases that record, propagates, and leaves later
  /// records.
  [[nodiscard]] std::size_t dispatch_ready();
  /// Releases ready observer obligations without invoking callbacks.
  /// Only one dispatch or discard call runs at once; competitors return zero.
  [[nodiscard]] std::size_t discard_ready();

private:
  struct State;
  explicit CommandRuntime(std::unique_ptr<State> state) noexcept;
  friend class Server;

  std::unique_ptr<State> state_;
};

/// Move-only result handle for one admitted command; its methods are not
/// concurrent. Destruction releases its result without cancelling the command.
/// It may outlive the runtime without retaining runtime threads.
class CommandOperation final {
public:
  CommandOperation(CommandOperation&& other) noexcept;
  CommandOperation& operator=(CommandOperation&& other) noexcept;
  CommandOperation(const CommandOperation&) = delete;
  CommandOperation& operator=(const CommandOperation&) = delete;
  ~CommandOperation();

  /// Consumes the same bounded answer `Server::run` gives.
  /// Waiting never dispatches the Server's global observer.
  [[nodiscard]] expected<std::string, CommandFailure> wait() &&;

  /// Releases the result obligation without cancelling the command.
  /// The Server's global observer remains a runtime obligation.
  void detach() && noexcept;

  /// Requests withdrawal; false means no live transport cancellation remains.
  /// The eventual result's delivery says whether tmux may have acted.
  [[nodiscard]] bool request_cancel();

private:
  struct State;
  explicit CommandOperation(std::unique_ptr<State> state) noexcept;
  friend class CommandRuntime;
  friend class Server;

  std::unique_ptr<State> state_;
};

LIBTMUX_NAMESPACE_END
