#include "process_engine.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

LIBTMUX_NAMESPACE_BEGIN
namespace detail {
namespace {

using Clock = ChildClock;

[[nodiscard]] FailureKind kind_of(ProcessError::Kind kind) noexcept {
  switch (kind) {
  case ProcessError::Kind::validation:
    return FailureKind::validation;
  case ProcessError::Kind::spawn:
    return FailureKind::spawn;
  case ProcessError::Kind::pre_exec:
    return FailureKind::pre_exec;
  case ProcessError::Kind::pipe:
    return FailureKind::pipe;
  case ProcessError::Kind::timeout:
    return FailureKind::timeout;
  }
  return FailureKind::spawn;
}

// What a caller sees. The engine's transport speaks ProcessError; an operation
// carries the installed failure type, so the boundary is here.
[[nodiscard]] CommandFailure reported(ProcessError error) {
  return CommandFailure{.kind = kind_of(error.kind),
                        .delivery = error.delivery,
                        .exit_code = -1,
                        .diagnostic = std::move(error.diagnostic)};
}

constexpr auto poll_quantum = std::chrono::milliseconds{10};
// Nothing left to ask after: the loop sleeps until something wakes it.
constexpr auto idle_quantum = std::chrono::milliseconds{500};
constexpr auto terminate_grace = std::chrono::milliseconds{100};
constexpr auto post_exit_drain = std::chrono::milliseconds{100};

[[nodiscard]] int poll_timeout(Clock::time_point boundary) {
  const auto now = Clock::now();
  if (now >= boundary) {
    return 0;
  }
  const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(boundary - now);
  return static_cast<int>(
      std::min<std::chrono::milliseconds::rep>(remaining.count(), 1000));
}

} // namespace

// The reactor is woken rather than polled: a cancellation or a detach has to
// reach it without waiting for the next turn.
class ProcessEngine::Hooks final : public OperationHooks {
public:
  explicit Hooks(std::weak_ptr<ProcessEngine> engine) noexcept
      : engine_{std::move(engine)} {}

  void wake_reactor() noexcept override {
    if (auto engine = engine_.lock()) {
      engine->wake();
    }
  }

  void release_admission() noexcept override {
    if (auto engine = engine_.lock()) {
      engine->release_admission();
    }
  }

private:
  std::weak_ptr<ProcessEngine> engine_;
};

expected<std::shared_ptr<ProcessEngine>, ProcessError>
ProcessEngine::start(EngineConfig config) {
  std::array<int, 2> wake{-1, -1};
#if defined(__linux__) && !defined(LIBTMUX_FORCE_PORTABLE_SYSCALLS)
  if (::pipe2(wake.data(), O_CLOEXEC | O_NONBLOCK) != 0) {
#else
  if (::pipe(wake.data()) != 0) {
#endif
    return unexpected(process_error(
        ProcessError::Kind::pipe, DeliveryStatus::not_started, "engine wake pipe",
        "process engine", std::error_code{errno, std::generic_category()}));
  }
#if !defined(__linux__) || defined(LIBTMUX_FORCE_PORTABLE_SYSCALLS)
  for (const int descriptor : wake) {
    const auto descriptor_flags = ::fcntl(descriptor, F_GETFD);
    const auto status_flags = ::fcntl(descriptor, F_GETFL);
    static_cast<void>(::fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC));
    static_cast<void>(::fcntl(descriptor, F_SETFL, status_flags | O_NONBLOCK));
  }
#endif
  std::shared_ptr<ProcessEngine> engine{new ProcessEngine{config, wake[0], wake[1]}};
  engine->launcher_ = std::thread{[engine] { engine->launch_loop(); }};
  engine->reactor_ = std::thread{[engine] { engine->reactor_loop(); }};
  return engine;
}

ProcessEngine::ProcessEngine(EngineConfig config, int wake_read,
                             int wake_write) noexcept
    : config_{config}, wake_read_{wake_read}, wake_write_{wake_write} {}

ProcessEngine::~ProcessEngine() {
  static_cast<void>(close());
  if (wake_read_ >= 0) {
    static_cast<void>(::close(wake_read_));
  }
  if (wake_write_ >= 0) {
    static_cast<void>(::close(wake_write_));
  }
}

void ProcessEngine::wake() noexcept {
  const char byte = 1;
  while (::write(wake_write_, &byte, 1) < 0 && errno == EINTR) {
  }
}

void ProcessEngine::drain_wake() noexcept {
  std::array<char, 64> buffer{};
  while (::read(wake_read_, buffer.data(), buffer.size()) > 0) {
  }
}

bool ProcessEngine::admit() noexcept {
  std::lock_guard lock{mutex_};
  if (closing_ || in_flight_ >= config_.operation_limit) {
    return false;
  }
  ++in_flight_;
  return true;
}

void ProcessEngine::release_admission() noexcept {
  std::lock_guard lock{mutex_};
  if (in_flight_ > 0U) {
    --in_flight_;
  }
}

Operation<ProcessReply> ProcessEngine::submit(ProcessRequest request) {
  auto hooks = std::make_shared<Hooks>(weak_from_this());
  auto started = make_operation<ProcessReply>(hooks);
  if (!admit()) {
    static_cast<void>(started.source.publish(unexpected(reported(process_error(
        ProcessError::Kind::validation, DeliveryStatus::not_started, "admission",
        "process engine",
        std::make_error_code(std::errc::resource_unavailable_try_again))))));
    started.source.retire();
    return std::move(started.operation);
  }
  started.source.mark_dispatching();
  std::optional<Clock::time_point> deadline;
  if (request.timeout.has_value()) {
    deadline = Clock::now() + *request.timeout;
  }
  {
    std::lock_guard lock{mutex_};
    pending_.push_back(EnginePending{.request = std::move(request),
                                     .source = std::move(started.source),
                                     .deadline = deadline});
  }
  launch_ready_.notify_one();
  return std::move(started.operation);
}

void ProcessEngine::launch_loop() {
  for (;;) {
    EnginePending work;
    {
      std::unique_lock lock{mutex_};
      launch_ready_.wait(lock, [this] { return closing_ || !pending_.empty(); });
      if (pending_.empty()) {
        return;
      }
      work = std::move(pending_.front());
      pending_.pop_front();
    }
    // Only creation happens here. Everything the child needs afterwards goes
    // with it to the reactor, so a launch that blocks stalls nothing else.
    auto launched = PosixChild::launch(work.request);
    if (!launched.has_value()) {
      static_cast<void>(
          work.source.publish(unexpected(reported(std::move(launched.error())))));
      work.source.retire();
      continue;
    }
    work.source.mark_active();
    {
      std::lock_guard lock{mutex_};
      arrived_.push_back(EngineLive{.child = std::move(*launched),
                                    .source = std::move(work.source),
                                    .deadline = work.deadline});
    }
    wake();
  }
}

void ProcessEngine::reactor_loop() {
  std::vector<EngineLive> live;
  for (;;) {
    {
      std::lock_guard lock{mutex_};
      for (auto& arrival : arrived_) {
        live.push_back(std::move(arrival));
      }
      arrived_.clear();
      if (closing_ && live.empty() && pending_.empty()) {
        return;
      }
    }

    std::vector<pollfd> watched;
    bool asks_after_exit = false;
    watched.push_back(pollfd{.fd = wake_read_, .events = POLLIN, .revents = 0});
    for (auto& one : live) {
      for (const auto stream :
           {ChildStream::stdout_stream, ChildStream::stderr_stream}) {
        if (one.child.descriptor(stream) >= 0) {
          watched.push_back(pollfd{
              .fd = one.child.descriptor(stream), .events = POLLIN, .revents = 0});
        }
      }
      // Where exit is a readable descriptor the loop waits for it. Where it
      // is not, the loop has to keep asking, and only then does it need a
      // turn of its own to ask on.
      if (one.child.exit_descriptor() >= 0) {
        watched.push_back(
            pollfd{.fd = one.child.exit_descriptor(), .events = POLLIN, .revents = 0});
      } else if (one.child.status() == ChildStatus::running) {
        asks_after_exit = true;
      }
    }
    auto boundary = Clock::now() + (asks_after_exit ? poll_quantum : idle_quantum);
    for (const auto& one : live) {
      if (one.deadline.has_value()) {
        boundary = std::min(boundary, *one.deadline);
      }
    }
    static_cast<void>(::poll(watched.data(), watched.size(), poll_timeout(boundary)));
    drain_wake();

    for (auto& one : live) {
      static_cast<void>(one.child.drain(ChildStream::stdout_stream, boundary,
                                        DeliveryStatus::indeterminate));
      static_cast<void>(one.child.drain(ChildStream::stderr_stream, boundary,
                                        DeliveryStatus::indeterminate));
      static_cast<void>(one.child.update_status(DeliveryStatus::indeterminate));

      const bool finished = one.child.status() != ChildStatus::running;
      if (finished && one.exit_drain_deadline == Clock::time_point::max()) {
        one.exit_drain_deadline = Clock::now() + post_exit_drain;
      }
      if (finished && Clock::now() >= one.exit_drain_deadline) {
        one.child.close_output();
      }
      const bool expired = one.deadline.has_value() && Clock::now() >= *one.deadline;
      const bool cancelled = one.source.cancel_requested();
      if (!finished && (expired || cancelled) && !one.terminate_deadline.has_value()) {
        one.child.signal_group(SIGTERM);
        one.terminate_deadline = Clock::now() + terminate_grace;
      }
      if (!finished && one.terminate_deadline.has_value() && !one.killed &&
          Clock::now() >= *one.terminate_deadline) {
        one.child.signal_group(SIGKILL);
        one.killed = true;
      }
    }

    // Deciding and removing in one pass. Two copies of the completion test
    // would let a child publish twice the moment they disagreed.
    std::erase_if(live, [&](EngineLive& one) {
      // Exit and the end of output are separate facts, and completion needs
      // both: a child that has exited may still have bytes in the pipe.
      if (one.child.status() == ChildStatus::running || !one.child.output_closed()) {
        return false;
      }
      auto capture = one.child.take_capture();
      if (one.child.status() == ChildStatus::unknowable) {
        static_cast<void>(one.source.publish(unexpected(reported(
            process_error(ProcessError::Kind::pipe, DeliveryStatus::written, "waitpid",
                          one.child.rendered_request(),
                          std::make_error_code(std::errc::no_child_process))))));
      } else if (one.terminate_deadline.has_value()) {
        static_cast<void>(one.source.publish(unexpected(reported(process_error(
            ProcessError::Kind::timeout, DeliveryStatus::indeterminate, "timeout",
            one.child.rendered_request(), std::make_error_code(std::errc::timed_out),
            std::move(capture))))));
      } else {
        static_cast<void>(one.source.publish(
            ProcessReply{.termination = one.child.termination(),
                         .stdout_bytes = std::move(capture.stdout_bytes),
                         .stderr_bytes = std::move(capture.stderr_bytes),
                         .output_truncated = capture.truncated}));
      }
      one.source.begin_retirement();
      one.source.retire();
      {
        std::lock_guard lock{mutex_};
        ++published_;
        ++reaped_;
      }
      return true;
    });
  }
}

EngineShutdown ProcessEngine::close() {
  {
    std::lock_guard lock{mutex_};
    if (closing_) {
      return EngineShutdown{published_, reaped_, pending_.empty()};
    }
    closing_ = true;
  }
  launch_ready_.notify_all();
  wake();
  if (launcher_.joinable()) {
    launcher_.join();
  }
  if (reactor_.joinable()) {
    reactor_.join();
  }
  std::lock_guard lock{mutex_};
  return EngineShutdown{published_, reaped_, true};
}

} // namespace detail
LIBTMUX_NAMESPACE_END
