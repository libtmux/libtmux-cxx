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
    static_cast<void>(::fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC));
    static_cast<void>(::fcntl(descriptor, F_SETFL, status_flags | O_NONBLOCK));
  }
#endif
  auto channel =
      std::make_shared<EngineChannel>(config.operation_limit, wake[0], wake[1]);
  // The threads hold a raw reference on purpose. Owning the engine would keep
  // it alive for as long as they run, which is until it is destroyed.
  std::shared_ptr<ProcessEngine> engine{new ProcessEngine{std::move(channel)}};
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

bool EngineChannel::admit() noexcept {
  std::lock_guard lock{mutex_};
  if (in_flight_ >= operation_limit_) {
    return false;
  }
  ++in_flight_;
  return true;
}

void EngineChannel::release() noexcept {
  std::lock_guard lock{mutex_};
  if (in_flight_ > 0U) {
    --in_flight_;
  }
}

void EngineChannel::wake() noexcept {
  const char byte = 1;
  while (::write(wake_write_, &byte, 1) < 0 && errno == EINTR) {
  }
}

void EngineChannel::drain() noexcept {
  std::array<char, 64> buffer{};
  while (::read(wake_read_, buffer.data(), buffer.size()) > 0) {
  }
}

ProcessEngine::ProcessEngine(std::shared_ptr<EngineChannel> channel) noexcept
    : channel_{std::move(channel)} {}

ProcessEngine::~ProcessEngine() { static_cast<void>(close()); }

Operation<ProcessReply> ProcessEngine::submit(ProcessRequest request) {
  if (!channel_->admit()) {
    auto refused = make_operation<ProcessReply>(std::make_shared<UnadmittedHooks>());
    static_cast<void>(refused.source.publish(unexpected(CommandFailure{
        .kind = FailureKind::overloaded,
        .delivery = DeliveryStatus::not_started,
        .exit_code = 0,
        .diagnostic = "the process engine has more work in flight than it accepts"})));
    refused.source.retire();
    return std::move(refused.operation);
  }
  auto started = make_operation<ProcessReply>(std::make_shared<ChannelHooks>(channel_));
  std::optional<Clock::time_point> deadline;
  if (request.timeout.has_value()) {
    deadline = Clock::now() + *request.timeout;
  }
  {
    std::lock_guard lock{mutex_};
    // Accepting under the lock the launch lane reads `closing_` under. Split
    // apart, a submission that admitted a moment before shutdown lands in a
    // queue nothing will ever read again.
    if (!closing_) {
      started.source.mark_dispatching();
      pending_.push_back(EnginePending{.request = std::move(request),
                                       .source = std::move(started.source),
                                       .deadline = deadline});
      launch_ready_.notify_one();
      return std::move(started.operation);
    }
  }
  static_cast<void>(started.source.publish(unexpected(CommandFailure{
      .kind = FailureKind::cancelled,
      .delivery = DeliveryStatus::not_started,
      .exit_code = 0,
      .diagnostic = "the process engine closed before this was accepted"})));
  started.source.retire();
  return std::move(started.operation);
}

void ProcessEngine::launch_loop() {
  for (;;) {
    EnginePending work;
    bool closing = false;
    {
      std::unique_lock lock{mutex_};
      launch_ready_.wait(lock, [this] { return closing_ || !pending_.empty(); });
      if (pending_.empty()) {
        return;
      }
      work = std::move(pending_.front());
      pending_.pop_front();
      closing = closing_;
      ++launching_;
    }
    launch_one(std::move(work), closing);
    {
      std::lock_guard lock{mutex_};
      --launching_;
    }
    // Whatever came of it, the reactor has a reason to look again: a child to
    // own, or one fewer launch standing between it and being finished.
    channel_->wake();
  }
}

void ProcessEngine::launch_one(EnginePending work, bool closing) {
  // Withdrawn before anything started, or closed before anything started:
  // either way nothing starts, and this is the one cancellation that costs a
  // caller nothing to retry. Starting it would run the command for real --
  // side effects and all -- to end it a moment later and report it cancelled.
  const bool withdrawn = work.source.cancel_requested();
  if (withdrawn || closing) {
    static_cast<void>(work.source.publish(unexpected(CommandFailure{
        .kind = FailureKind::cancelled,
        .delivery = DeliveryStatus::not_started,
        .exit_code = 0,
        .diagnostic = withdrawn ? "the caller withdrew the command before it started"
                                : "the process engine closed before this started"})));
    work.source.retire();
    return;
  }
  // Only creation happens here. Everything the child needs afterwards goes
  // with it to the reactor, so a launch that blocks stalls nothing else.
  auto launched = PosixChild::launch(work.request);
  if (!launched.has_value()) {
    static_cast<void>(
        work.source.publish(unexpected(reported(std::move(launched.error())))));
    work.source.retire();
    return;
  }
  work.source.mark_active();
  std::lock_guard lock{mutex_};
  arrived_.push_back(EngineLive{.child = std::move(*launched),
                                .source = std::move(work.source),
                                .deadline = work.deadline});
}

void ProcessEngine::reactor_loop() {
  std::vector<EngineLive> live;
  bool shutting_down = false;
  for (;;) {
    {
      std::lock_guard lock{mutex_};
      for (auto& arrival : arrived_) {
        live.push_back(std::move(arrival));
      }
      arrived_.clear();
      if (closing_ && live.empty() && pending_.empty() && launching_ == 0U) {
        return;
      }
      shutting_down = closing_;
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
      if (one.terminate_deadline.has_value()) {
        asks_after_exit = true;
      }
    }
    auto boundary = Clock::now() + (asks_after_exit ? poll_quantum : idle_quantum);
    for (const auto& one : live) {
      if (one.deadline.has_value()) {
        boundary = std::min(boundary, *one.deadline);
      }
    }
    // Children are signalled after this wait, so a closing engine must not
    // enter it: the decision to end them is already made, and sleeping on it
    // is what made teardown cost a whole quantum.
    if (shutting_down) {
      boundary = Clock::now();
    }
    static_cast<void>(::poll(watched.data(), static_cast<nfds_t>(watched.size()),
                             poll_timeout(boundary)));
    channel_->drain();

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
      // Closing ends what is running rather than waiting for it: a teardown
      // that waits takes as long as the slowest command anyone had in flight.
      if (!finished && (expired || cancelled || shutting_down) &&
          !one.terminate_deadline.has_value()) {
        one.child.signal_group(SIGTERM);
        one.terminate_deadline = Clock::now() + terminate_grace;
        one.withdrawn = cancelled && !expired;
        one.abandoned = shutting_down && !cancelled && !expired;
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
  channel_->wake();
  if (launcher_.joinable()) {
    launcher_.join();
  }
  if (reactor_.joinable()) {
    reactor_.join();
  }
  std::lock_guard lock{mutex_};
  return EngineShutdown{published_, reaped_, true};
}

expected<std::shared_ptr<ProcessEngine>, ProcessError> shared_engine() {
  static std::mutex guard;
  static std::weak_ptr<ProcessEngine> held;
  std::lock_guard lock{guard};
  if (auto engine = held.lock()) {
    return engine;
  }
  auto started = ProcessEngine::start();
  if (!started.has_value()) {
    return unexpected(std::move(started.error()));
  }
  held = *started;
  return started;
}

} // namespace detail
LIBTMUX_NAMESPACE_END
