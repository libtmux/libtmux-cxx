#pragma once

// Bounded asynchronous command execution.

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

struct CommandRuntimeConfig final {
  std::size_t capacity{256U};
};

struct CommandRuntimeSnapshot final {
  std::size_t capacity{};
  std::size_t in_flight{};
  std::size_t pending_results{};
  std::size_t pending_observers{};
  std::uint64_t accepted{};
  std::uint64_t refused{};
  std::uint64_t completed{};
  bool accepting{};
};

struct CommandRuntimeShutdown final {
  std::size_t pending_results{};
  std::size_t pending_observers{};
  bool transports_stopped{};
  bool safe_to_unload{};
  std::optional<CommandFailure> failure;
};

class CommandRuntime final {
public:
  static expected<CommandRuntime, CommandFailure>
  start(CommandRuntimeConfig config = {});
  CommandRuntime(CommandRuntime&&) noexcept;
  CommandRuntime& operator=(CommandRuntime&&) noexcept;
  CommandRuntime(const CommandRuntime&) = delete;
  CommandRuntime& operator=(const CommandRuntime&) = delete;
  ~CommandRuntime();

  void request_stop() noexcept;
  // Join every owned transport thread without invoking or discarding global
  // observers. The first terminal report is retained for later close calls.
  [[nodiscard]] CommandRuntimeShutdown close();
  [[nodiscard]] CommandRuntimeSnapshot snapshot() const noexcept;
  [[nodiscard]] std::size_t dispatch_ready();
  [[nodiscard]] std::size_t discard_ready();

private:
  struct State;
  explicit CommandRuntime(std::unique_ptr<State> state) noexcept;
  friend class Server;

  std::unique_ptr<State> state_;
};

class CommandOperation final {
public:
  CommandOperation(CommandOperation&& other) noexcept;
  CommandOperation& operator=(CommandOperation&& other) noexcept;
  CommandOperation(const CommandOperation&) = delete;
  CommandOperation& operator=(const CommandOperation&) = delete;
  ~CommandOperation();

  // What tmux said, once it has said it. The same answer `Server::run` gives,
  // including the bound on how much of it is kept and what a non-zero exit
  // means. Consumed once: the operation is spent afterwards.
  [[nodiscard]] expected<std::string, CommandFailure> wait() &&;

  // Stop retaining this result without cancelling its command or discarding
  // the runtime's global observation.
  void detach() && noexcept;

  // Ask for the command to be withdrawn. A request rather than an outcome:
  // tmux may answer first, and whether it acted is reported by the failure's
  // delivery rather than guessed at here.
  [[nodiscard]] bool request_cancel();

private:
  struct State;
  explicit CommandOperation(std::unique_ptr<State> state) noexcept;
  friend class CommandRuntime;
  friend class Server;

  std::unique_ptr<State> state_;
};

LIBTMUX_NAMESPACE_END
