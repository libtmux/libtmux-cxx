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
  case ProcessError::Kind::cancelled:
    return FailureKind::cancelled;
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

// An operation the engine never admitted has no slot to give back and no
// reactor to wake. Wiring it to the engine instead would return admission it
// never took, and a bound that leaks a slot per refusal stops bounding.
class UnadmittedHooks final : public OperationHooks {
public:
  void wake_reactor() noexcept override {}
  void release_admission() noexcept override {}
};

// The reactor is woken rather than polled: a cancellation or a detach has to
// reach it without waiting for the next turn.
class ChannelHooks final : public OperationHooks {
public:
  explicit ChannelHooks(std::shared_ptr<EngineChannel> channel) noexcept
      : channel_{std::move(channel)} {}

  void wake_reactor() noexcept override { channel_->wake(); }
  void release_admission() noexcept override { channel_->release(); }

private:
  std::shared_ptr<EngineChannel> channel_;
};

[[nodiscard]] int poll_timeout(Clock::time_point boundary) {
  const auto now = Clock::now();
  if (now >= boundary) {
    return 0;
  }
  const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(boundary - now);
  return static_cast<int>(
      std::min<std::chrono::milliseconds::rep>(remaining.count(), 1000));
}

void retire_transport(OperationSource<ProcessReply>& source,
                      MoveOnlyFunction<void()>& retirement_hook) {
  source.retire();
  if (retirement_hook) {
    auto hook = std::move(retirement_hook);
    hook();
  }
}

void observe_reactor(const std::function<void(EngineReactorEvent)>& observer,
                     EngineReactorEvent event) noexcept {
  if (!observer) {
    return;
  }
  try {
    observer(event);
  } catch (...) {
    // A test observer cannot be allowed to terminate the reactor.
  }
}

class ReactorExitObservation final {
public:
  explicit ReactorExitObservation(
      const std::function<void(EngineReactorEvent)>& observer) noexcept
      : observer_{observer} {}

  ~ReactorExitObservation() { observe_reactor(observer_, EngineReactorEvent::exited); }

  ReactorExitObservation(const ReactorExitObservation&) = delete;
  ReactorExitObservation& operator=(const ReactorExitObservation&) = delete;

private:
  const std::function<void(EngineReactorEvent)>& observer_;
};

} // namespace

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
    if (descriptor_flags < 0 || status_flags < 0 ||
        ::fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0 ||
        ::fcntl(descriptor, F_SETFL, status_flags | O_NONBLOCK) != 0) {
      const auto error_number = errno;
      static_cast<void>(::close(wake[0]));
      static_cast<void>(::close(wake[1]));
      return unexpected(process_error(
          ProcessError::Kind::pipe, DeliveryStatus::not_started, "engine wake fcntl",
          "process engine", std::error_code{error_number, std::generic_category()}));
    }
  }
#endif
  auto channel =
      std::make_shared<EngineChannel>(config.operation_limit, wake[0], wake[1]);
  // The threads hold a raw reference on purpose. Owning the engine would keep
  // it alive for as long as they run, which is until it is destroyed.
  std::shared_ptr<ProcessEngine> engine{new ProcessEngine{
      std::move(channel), std::move(config.admission_gate),
      std::move(config.launch_observer), std::move(config.reactor_observer)}};
  engine->launcher_ = std::thread{[owner = engine.get()] { owner->launch_loop(); }};
  engine->reactor_ = std::thread{[owner = engine.get()] { owner->reactor_loop(); }};
  return engine;
}

EngineChannel::EngineChannel(std::size_t operation_limit, int wake_read,
                             int wake_write) noexcept
    : operation_limit_{operation_limit}, wake_read_{wake_read},
      wake_write_{wake_write} {}

EngineChannel::~EngineChannel() {
  if (wake_read_ >= 0) {
    static_cast<void>(::close(wake_read_));
  }
  if (wake_write_ >= 0) {
    static_cast<void>(::close(wake_write_));
  }
}

EngineAdmission EngineChannel::admit() noexcept {
  std::lock_guard lock{mutex_};
  if (failure_) {
    return EngineAdmission{.failure = failure_};
  }
  if (in_flight_ >= operation_limit_) {
    return {};
  }
  ++in_flight_;
  return EngineAdmission{.accepted = true};
}

void EngineChannel::release() noexcept {
  std::lock_guard lock{mutex_};
  if (in_flight_ > 0U) {
    --in_flight_;
  }
}

EngineLaunchGate EngineChannel::gate_launch() noexcept {
  std::unique_lock lock{mutex_};
  return EngineLaunchGate{std::move(lock), failure_};
}

void EngineChannel::wake() noexcept {
  const char byte = 1;
  for (;;) {
    const auto count = ::write(wake_write_, &byte, 1);
    if (count == 1) {
      return;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return;
    }
    fail("engine wake write", count < 0 ? errno : EIO);
    return;
  }
}

void EngineChannel::drain() noexcept {
  std::array<char, 64> buffer{};
  for (;;) {
    const auto count = ::read(wake_read_, buffer.data(), buffer.size());
    if (count > 0) {
      continue;
    }
    if (count == 0) {
      fail("engine wake read", EPIPE);
      return;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }
    fail("engine wake read", errno);
    return;
  }
}

void EngineChannel::fail(std::string_view operation, int error_number) noexcept {
  std::lock_guard lock{mutex_};
  if (!failure_) {
    failure_ =
        EngineFailure{.operation = operation,
                      .cause = std::error_code{error_number, std::generic_category()}};
  }
}

std::optional<EngineFailure> EngineChannel::failure() const noexcept {
  std::lock_guard lock{mutex_};
  return failure_;
}

ProcessEngine::ProcessEngine(
    std::shared_ptr<EngineChannel> channel, std::function<void()> admission_gate,
    std::function<void(const ProcessRequest&)> launch_observer,
    std::function<void(EngineReactorEvent)> reactor_observer) noexcept
    : channel_{std::move(channel)}, admission_gate_{std::move(admission_gate)},
      launch_observer_{std::move(launch_observer)},
      reactor_observer_{std::move(reactor_observer)} {}

ProcessEngine::~ProcessEngine() { static_cast<void>(close()); }

Operation<ProcessReply>
ProcessEngine::submit(ProcessRequest request,
                      MoveOnlyFunction<void()> retirement_hook) {
  const auto admission = channel_->admit();
  if (!admission.accepted) {
    auto refused = make_operation<ProcessReply>(std::make_shared<UnadmittedHooks>());
    if (admission.failure) {
      fail(*admission.failure);
      static_cast<void>(refused.source.publish(unexpected(
          reported(process_error(ProcessError::Kind::pipe, DeliveryStatus::not_started,
                                 admission.failure->operation, render_request(request),
                                 admission.failure->cause)))));
    } else {
      static_cast<void>(refused.source.publish(unexpected(CommandFailure{
          .kind = FailureKind::overloaded,
          .delivery = DeliveryStatus::not_started,
          .exit_code = 0,
          .diagnostic =
              "the process engine has more work in flight than it accepts"})));
    }
    retire_transport(refused.source, retirement_hook);
    return std::move(refused.operation);
  }
  // Test coordination belongs after admission: this is the interval a fatal
  // wake may cross before the launch lane sees the queued request.
  if (admission_gate_) {
    admission_gate_();
  }
  auto started = make_operation<ProcessReply>(std::make_shared<ChannelHooks>(channel_));
  std::optional<Clock::time_point> deadline;
  if (request.timeout.has_value()) {
    deadline = Clock::now() + *request.timeout;
  }
  std::optional<EngineFailure> terminal_failure;
  {
    std::lock_guard lock{mutex_};
    // Admission and the stop request share this lock. Split apart, a submission
    // accepted during shutdown could land in a queue nothing reads again.
    if (!stop_requested_ && !failure_) {
      started.source.mark_dispatching();
      pending_.push_back(EnginePending{.request = std::move(request),
                                       .source = std::move(started.source),
                                       .deadline = deadline,
                                       .retirement_hook = std::move(retirement_hook)});
      launch_ready_.notify_one();
      return std::move(started.operation);
    }
    terminal_failure = failure_;
  }
  if (terminal_failure) {
    static_cast<void>(started.source.publish(unexpected(
        reported(process_error(ProcessError::Kind::pipe, DeliveryStatus::not_started,
                               terminal_failure->operation, render_request(request),
                               terminal_failure->cause)))));
    retire_transport(started.source, retirement_hook);
    return std::move(started.operation);
  }
  static_cast<void>(started.source.publish(unexpected(CommandFailure{
      .kind = FailureKind::cancelled,
      .delivery = DeliveryStatus::not_started,
      .exit_code = 0,
      .diagnostic = "the process engine closed before this was accepted"})));
  retire_transport(started.source, retirement_hook);
  return std::move(started.operation);
}

void ProcessEngine::launch_loop() {
  for (;;) {
    EnginePending work;
    bool stopping = false;
    std::optional<EngineFailure> failure;
    {
      std::unique_lock lock{mutex_};
      launch_ready_.wait(
          lock, [this] { return stop_requested_ || failure_ || !pending_.empty(); });
      if (pending_.empty()) {
        return;
      }
      work = std::move(pending_.front());
      pending_.pop_front();
      stopping = stop_requested_;
      failure = failure_;
      ++launching_;
    }
    launch_one(std::move(work), stopping, std::move(failure));
    {
      std::lock_guard lock{mutex_};
      --launching_;
    }
    // Whatever came of it, the reactor has a reason to look again: a child to
    // own, or one fewer launch standing between it and being finished.
    channel_->wake();
    if (auto channel_failure = channel_->failure()) {
      fail(*channel_failure);
    }
  }
}

void ProcessEngine::launch_one(EnginePending work, bool stopping,
                               std::optional<EngineFailure> failure) {
  // Withdrawn before anything started, or closed before anything started:
  // either way nothing starts, and this is the one cancellation that costs a
  // caller nothing to retry. Starting it would run the command for real --
  // side effects and all -- to end it a moment later and report it cancelled.
  const bool withdrawn = work.source.cancel_requested();
  if (failure) {
    static_cast<void>(work.source.publish(unexpected(reported(process_error(
        ProcessError::Kind::pipe, DeliveryStatus::not_started, failure->operation,
        render_request(work.request), failure->cause)))));
    retire_transport(work.source, work.retirement_hook);
    return;
  }
  if (withdrawn || stopping) {
    static_cast<void>(work.source.publish(unexpected(CommandFailure{
        .kind = FailureKind::cancelled,
        .delivery = DeliveryStatus::not_started,
        .exit_code = 0,
        .diagnostic = withdrawn ? "the caller withdrew the command before it started"
                                : "the process engine closed before this started"})));
    retire_transport(work.source, work.retirement_hook);
    return;
  }
  if (work.deadline.has_value() && Clock::now() >= *work.deadline) {
    static_cast<void>(work.source.publish(unexpected(reported(process_error(
        ProcessError::Kind::timeout, DeliveryStatus::not_started, "timeout",
        render_request(work.request), std::make_error_code(std::errc::timed_out))))));
    retire_transport(work.source, work.retirement_hook);
    return;
  }
  if (launch_observer_) {
    try {
      launch_observer_(work.request);
    } catch (...) {
      // A test observer cannot be allowed to terminate the launch lane.
    }
  }
  auto launch_gate = channel_->gate_launch();
  if (launch_gate.failure()) {
    const auto channel_failure = *launch_gate.failure();
    launch_gate.unlock();
    static_cast<void>(work.source.publish(unexpected(
        reported(process_error(ProcessError::Kind::pipe, DeliveryStatus::not_started,
                               channel_failure.operation, render_request(work.request),
                               channel_failure.cause)))));
    retire_transport(work.source, work.retirement_hook);
    return;
  }
  // Only creation happens here. Everything the child needs afterwards goes
  // with it to the reactor, so a launch that blocks stalls nothing else.
  auto launched = PosixChild::launch(work.request);
  launch_gate.unlock();
  if (!launched.has_value()) {
    static_cast<void>(
        work.source.publish(unexpected(reported(std::move(launched.error())))));
    retire_transport(work.source, work.retirement_hook);
    return;
  }
  work.source.mark_active();
  std::optional<ProcessError> teardown_failure;
  if (const auto teardown = launched->spawn_teardown_error(); teardown != 0) {
    teardown_failure =
        process_error(ProcessError::Kind::pipe, DeliveryStatus::indeterminate, "pipe",
                      launched->rendered_request(),
                      std::error_code{teardown, std::generic_category()});
  }
  std::lock_guard lock{mutex_};
  arrived_.push_back(EngineLive{.child = std::move(*launched),
                                .source = std::move(work.source),
                                .deadline = work.deadline,
                                .failure = std::move(teardown_failure),
                                .retirement_hook = std::move(work.retirement_hook)});
}

void ProcessEngine::fail(EngineFailure failure) {
  bool stored = false;
  {
    std::lock_guard lock{mutex_};
    if (!failure_) {
      failure_ = failure;
      stored = true;
    }
  }
  if (stored) {
    launch_ready_.notify_all();
    channel_->wake();
  }
}

void ProcessEngine::reactor_loop() {
  ReactorExitObservation observe_exit{reactor_observer_};
  std::vector<EngineLive> live;
  bool launch_wait_observed = false;
  for (;;) {
    if (auto channel_failure = channel_->failure()) {
      fail(*channel_failure);
    }
    bool stopping = false;
    bool waiting_for_launch = false;
    std::optional<EngineFailure> engine_failure;
    {
      std::lock_guard lock{mutex_};
      for (auto& arrival : arrived_) {
        live.push_back(std::move(arrival));
      }
      arrived_.clear();
      if ((stop_requested_ || failure_) && live.empty() && pending_.empty() &&
          launching_ == 0U) {
        return;
      }
      waiting_for_launch = (stop_requested_ || failure_) && live.empty() &&
                           pending_.empty() && launching_ != 0U;
      stopping = stop_requested_;
      engine_failure = failure_;
    }
    if (waiting_for_launch && !launch_wait_observed) {
      observe_reactor(reactor_observer_, EngineReactorEvent::waiting_for_launch);
      launch_wait_observed = true;
    }

    std::vector<pollfd> watched;
    bool asks_after_exit = false;
    watched.push_back(
        pollfd{.fd = channel_->wake_descriptor(), .events = POLLIN, .revents = 0});
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
      // A child being ended is checked on the short turn whatever else is in
      // the set: it is about to be signalled again, reaped and published.
      if (one.failure || one.terminate_deadline.has_value()) {
        asks_after_exit = true;
      }
    }
    auto boundary = Clock::now() + (asks_after_exit ? poll_quantum : idle_quantum);
    for (const auto& one : live) {
      if (one.deadline.has_value()) {
        boundary = std::min(boundary, *one.deadline);
      }
      if (one.exit_drain_deadline != Clock::time_point::max()) {
        boundary = std::min(boundary, one.exit_drain_deadline);
      }
      if (one.terminate_deadline.has_value()) {
        boundary = std::min(boundary, *one.terminate_deadline);
      }
    }
    // Children are signalled after this wait, so a closing engine must not
    // enter it: the decision to end them is already made, and sleeping on it
    // is what made teardown cost a whole quantum.
    if (stopping || engine_failure) {
      boundary = Clock::now();
    }
    int poll_result = 0;
    do {
      poll_result = ::poll(watched.data(), static_cast<nfds_t>(watched.size()),
                           poll_timeout(boundary));
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result < 0) {
      const auto error_number = errno;
      channel_->fail("pipe poll", error_number);
      if (auto channel_failure = channel_->failure()) {
        fail(*channel_failure);
      }
    } else {
      channel_->drain();
      if (auto channel_failure = channel_->failure()) {
        fail(*channel_failure);
      }
    }
    {
      std::lock_guard lock{mutex_};
      engine_failure = failure_;
    }

    for (auto& one : live) {
      const auto retain = [&one](std::optional<ProcessError> failure) {
        if (failure && !one.failure) {
          one.failure = std::move(*failure);
        }
      };
      if (engine_failure && !one.failure) {
        one.failure =
            process_error(ProcessError::Kind::pipe, DeliveryStatus::indeterminate,
                          engine_failure->operation, one.child.rendered_request(),
                          engine_failure->cause);
      }
      retain(one.child.drain_once(ChildStream::stdout_stream, boundary,
                                  DeliveryStatus::indeterminate));
      retain(one.child.drain_once(ChildStream::stderr_stream, boundary,
                                  DeliveryStatus::indeterminate));
      retain(one.child.update_status(DeliveryStatus::indeterminate));

      bool finished = one.child.status() != ChildStatus::running;
      const bool expired = one.deadline.has_value() && Clock::now() >= *one.deadline;
      const bool cancelled = one.source.cancel_requested();
      // Closing ends what is running rather than waiting for it: a teardown
      // that waits takes as long as the slowest command anyone had in flight.
      if (!finished && !one.failure && (expired || cancelled || stopping) &&
          !one.terminate_deadline.has_value()) {
        auto signal_failure =
            one.child.signal_group(SIGTERM, DeliveryStatus::indeterminate);
        one.terminate_deadline =
            Clock::now() + (signal_failure ? Clock::duration::zero() : terminate_grace);
        retain(std::move(signal_failure));
        one.withdrawn = cancelled && !expired;
        one.abandoned = stopping && !cancelled && !expired;
      }
      if (!finished && !one.failure && one.terminate_deadline.has_value() &&
          !one.killed && Clock::now() >= *one.terminate_deadline) {
        auto signal_failure =
            one.child.signal_group(SIGKILL, DeliveryStatus::indeterminate);
        one.killed = !signal_failure;
        retain(std::move(signal_failure));
      }
      if (!finished && one.failure && !one.killed) {
        auto signal_failure =
            one.child.signal_group(SIGKILL, DeliveryStatus::indeterminate);
        one.killed = !signal_failure;
        retain(std::move(signal_failure));
      }
      if (!finished && one.killed) {
        retain(one.child.wait_for_exit(DeliveryStatus::indeterminate));
      }

      finished = one.child.status() != ChildStatus::running;
      if (finished && one.exit_drain_deadline == Clock::time_point::max()) {
        one.exit_drain_deadline = Clock::now() + post_exit_drain;
      }
      if (finished && Clock::now() >= one.exit_drain_deadline) {
        one.child.close_output();
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
      if (one.failure) {
        one.failure->stdout_bytes = std::move(capture.stdout_bytes);
        one.failure->stderr_bytes = std::move(capture.stderr_bytes);
        one.failure->output_truncated = capture.truncated;
        static_cast<void>(
            one.source.publish(unexpected(reported(std::move(*one.failure)))));
      } else if (one.child.status() == ChildStatus::unknowable) {
        static_cast<void>(one.source.publish(unexpected(reported(
            process_error(ProcessError::Kind::pipe, DeliveryStatus::written, "waitpid",
                          one.child.rendered_request(),
                          std::make_error_code(std::errc::no_child_process))))));
      } else if (one.abandoned) {
        static_cast<void>(one.source.publish(unexpected(CommandFailure{
            .kind = FailureKind::cancelled,
            .delivery = DeliveryStatus::indeterminate,
            .exit_code = 0,
            .diagnostic = "the process engine closed while this was running"})));
      } else if (one.withdrawn) {
        static_cast<void>(one.source.publish(unexpected(CommandFailure{
            .kind = FailureKind::cancelled,
            .delivery = DeliveryStatus::indeterminate,
            .exit_code = 0,
            .diagnostic = "the caller withdrew the command after it started"})));
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
      retire_transport(one.source, one.retirement_hook);
      {
        std::lock_guard lock{mutex_};
        ++published_;
        if (one.child.status() == ChildStatus::exited) {
          ++reaped_;
        }
      }
      return true;
    });
  }
}

void ProcessEngine::request_stop() noexcept {
  {
    std::lock_guard lock{mutex_};
    stop_requested_ = true;
  }
  launch_ready_.notify_all();
  channel_->wake();
}

EngineShutdown ProcessEngine::close() {
  {
    std::unique_lock lock{mutex_};
    if (terminal_) {
      return terminal_shutdown_;
    }
    if (close_joining_) {
      terminal_ready_.wait(lock, [this] { return terminal_; });
      return terminal_shutdown_;
    }
    stop_requested_ = true;
    close_joining_ = true;
  }
  launch_ready_.notify_all();
  channel_->wake();
  if (launcher_.joinable()) {
    launcher_.join();
  }
  if (reactor_.joinable()) {
    reactor_.join();
  }
  EngineShutdown report;
  {
    std::lock_guard lock{mutex_};
    terminal_shutdown_ = EngineShutdown{published_, reaped_, published_ == reaped_};
    terminal_ = true;
    report = terminal_shutdown_;
  }
  terminal_ready_.notify_all();
  return report;
}

} // namespace detail
LIBTMUX_NAMESPACE_END
