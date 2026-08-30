#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "libtmux/command.hpp"
#include "libtmux/expected.hpp"
#include "move_only_function.hpp"
#include "operation_state.hpp"

LIBTMUX_NAMESPACE_BEGIN
namespace detail {

class SubprocessBackend;

struct CommandEngineConfig final {
  std::size_t operation_limit{256U};
  std::size_t worker_count{2U};
};

class CommandChannel final {
public:
  explicit CommandChannel(std::size_t operation_limit) noexcept;

  [[nodiscard]] bool admit() noexcept;
  void release() noexcept;

private:
  std::mutex mutex_;
  std::size_t operation_limit_;
  std::size_t in_flight_{0U};
};

struct PendingCommand final {
  std::shared_ptr<const SubprocessBackend> backend;
  CommandRequest command;
  std::optional<std::chrono::milliseconds> timeout;
  std::optional<std::size_t> output_limit;
  OperationSource<std::string> source;
  std::optional<std::chrono::steady_clock::time_point> deadline;
  MoveOnlyFunction<void()> retirement_hook;
};

class CommandEngine final {
public:
  [[nodiscard]] static expected<std::shared_ptr<CommandEngine>, CommandFailure>
  start(CommandEngineConfig config = {});

  ~CommandEngine();
  CommandEngine(const CommandEngine&) = delete;
  CommandEngine& operator=(const CommandEngine&) = delete;

  [[nodiscard]] Operation<std::string>
  submit(std::shared_ptr<const SubprocessBackend> backend, CommandRequest command,
         std::optional<std::chrono::milliseconds> timeout,
         std::optional<std::size_t> output_limit,
         MoveOnlyFunction<void()> retirement_hook = {});

  void request_stop() noexcept;
  void close();

private:
  explicit CommandEngine(std::shared_ptr<CommandChannel> channel) noexcept;

  void worker_loop();
  std::shared_ptr<CommandChannel> channel_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<PendingCommand> pending_;
  bool stop_requested_{false};
  bool close_joining_{false};
  bool terminal_{false};
  std::condition_variable terminal_ready_;
  std::vector<std::thread> workers_;
};

} // namespace detail
LIBTMUX_NAMESPACE_END
