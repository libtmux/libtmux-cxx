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
  // Ended because the engine is closing, which is neither.
  bool abandoned{false};
};

// What a hook may still be holding once the engine is gone.
//
// The engine's threads must not own the engine, or its shutdown never runs.
// Nor may a hook revive it: `release_admission` is reached from the reactor,
// and reviving the engine there would leave the reactor joining itself. So
// the two things a hook reaches for -- the wake pipe and the admission count
// -- live here, where holding them keeps no thread alive and revives nothing.
class EngineChannel final {
public:
  EngineChannel(std::size_t operation_limit, int wake_read, int wake_write) noexcept;
  ~EngineChannel();
  EngineChannel(const EngineChannel&) = delete;
  EngineChannel& operator=(const EngineChannel&) = delete;

  // Accepted work in flight, against the bound. A refused caller is told at
  // once rather than queued behind a limit it cannot see.
  [[nodiscard]] bool admit() noexcept;
  void release() noexcept;

  void wake() noexcept;
  void drain() noexcept;
  [[nodiscard]] int wake_descriptor() const noexcept { return wake_read_; }

private:
  std::mutex mutex_;
  std::size_t operation_limit_;
  std::size_t in_flight_{0U};
  int wake_read_{-1};
  int wake_write_{-1};
};

class ProcessEngine final {
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
  explicit ProcessEngine(std::shared_ptr<EngineChannel> channel) noexcept;

  void launch_loop();
  void launch_one(EnginePending work);
  void reactor_loop();

  std::shared_ptr<EngineChannel> channel_;

  std::mutex mutex_;
  std::condition_variable launch_ready_;
  std::deque<EnginePending> pending_;
  std::vector<EngineLive> arrived_;
  // Work the launch lane has taken but not yet handed over. It is in neither
  // queue while the platform creates the process, and a reactor that read
  // that gap as "nothing left" would return and strand a live child.
  std::size_t launching_{0U};
  bool closing_{false};

  std::size_t published_{0U};
  std::size_t reaped_{0U};

  std::thread launcher_;
  std::thread reactor_;
};

// One engine for the process, not one for each Server.
//
// It exists while some caller holds it and is gone when the last lets go, so
// the two threads are the cost of using this library at all rather than a toll
// on every connection opened. Nothing static owns it: the weak reference below
// holds no engine, so there is no destruction order to get wrong.
[[nodiscard]] expected<std::shared_ptr<ProcessEngine>, ProcessError> shared_engine();

} // namespace detail
LIBTMUX_NAMESPACE_END
