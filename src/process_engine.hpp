#pragma once

// Running tmux without waiting for it.
//
// Two threads and no more, whatever the load. A launch lane does process
// creation only, so a platform call that blocks cannot stall anything already
// running. A reactor owns every live child: its pipes, its deadline, its exit,
// and the one publication that fixes each caller's outcome.
//
// A submission completes when the child has exited *and* its output has ended.
// Those are separate facts and the earlier one is not an answer, which is the
// invariant CPython's subprocess transport enforces in `_try_finish`.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "libtmux/expected.hpp"
#include "operation_state.hpp"
#include "posix_child.hpp"
#include "process.hpp"

LIBTMUX_NAMESPACE_BEGIN
namespace detail {

struct EngineConfig final {
  // Accepted work in flight. Submission past this is refused rather than
  // queued, so a caller learns immediately instead of waiting behind a bound
  // it cannot see.
  std::size_t operation_limit{256U};
};

// What shutdown could not finish, so a caller is told rather than assured.
struct EngineShutdown final {
  std::size_t operations_published{0U};
  std::size_t children_reaped{0U};
  bool complete{false};
};

// A submission the launch lane has not reached yet.
struct EnginePending final {
  ProcessRequest request;
  OperationSource<ProcessReply> source;
  std::optional<ChildClock::time_point> deadline;
};

// A child the reactor owns.
struct EngineLive final {
  PosixChild child;
  OperationSource<ProcessReply> source;
  std::optional<ChildClock::time_point> deadline;
  ChildClock::time_point exit_drain_deadline{ChildClock::time_point::max()};
  std::optional<ChildClock::time_point> terminate_deadline{};
  bool killed{false};
  // Ended because the caller withdrew, not because a deadline passed.
  bool withdrawn{false};
};

class ProcessEngine final : public std::enable_shared_from_this<ProcessEngine> {
public:
  [[nodiscard]] static expected<std::shared_ptr<ProcessEngine>, ProcessError>
  start(EngineConfig config = {});

  ~ProcessEngine();
  ProcessEngine(const ProcessEngine&) = delete;
  ProcessEngine& operator=(const ProcessEngine&) = delete;

  // Never waits. A closed engine or a full admission bound answers at once
  // with an operation that is already published.
  [[nodiscard]] Operation<ProcessReply> submit(ProcessRequest request);

  [[nodiscard]] EngineShutdown close();

private:
  class Hooks;

  explicit ProcessEngine(EngineConfig config, int wake_read, int wake_write) noexcept;

  void launch_loop();
  void reactor_loop();
  void wake() noexcept;
  void drain_wake() noexcept;
  [[nodiscard]] bool admit() noexcept;
  void release_admission() noexcept;

  EngineConfig config_;
  int wake_read_{-1};
  int wake_write_{-1};

  std::mutex mutex_;
  std::condition_variable launch_ready_;
  std::deque<EnginePending> pending_;
  std::vector<EngineLive> arrived_;
  std::size_t in_flight_{0U};
  bool closing_{false};

  std::size_t published_{0U};
  std::size_t reaped_{0U};

  std::thread launcher_;
  std::thread reactor_;
};

} // namespace detail
LIBTMUX_NAMESPACE_END
