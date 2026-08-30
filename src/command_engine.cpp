#include "command_engine.hpp"

#include "backend.hpp"

#include <utility>

LIBTMUX_NAMESPACE_BEGIN
namespace detail {
namespace {

using Clock = std::chrono::steady_clock;

class ChannelHooks final : public OperationHooks {
public:
  explicit ChannelHooks(std::shared_ptr<CommandChannel> channel) noexcept
      : channel_{std::move(channel)} {}

  void wake_reactor() noexcept override {}
  void release_admission() noexcept override { channel_->release(); }

private:
  std::shared_ptr<CommandChannel> channel_;
};

class UnadmittedHooks final : public OperationHooks {
public:
  void wake_reactor() noexcept override {}
  void release_admission() noexcept override {}
};

void retire(OperationSource<std::string>& source,
            MoveOnlyFunction<void()>& retirement_hook) {
  source.retire();
  if (retirement_hook) {
    auto hook = std::move(retirement_hook);
    hook();
  }
}

[[nodiscard]] CommandFailure cancelled(DeliveryStatus delivery,
                                       std::string diagnostic) {
  return CommandFailure{.kind = FailureKind::cancelled,
                        .delivery = delivery,
                        .exit_code = 0,
                        .diagnostic = std::move(diagnostic)};
}

[[nodiscard]] CommandFailure timed_out() {
  return CommandFailure{.kind = FailureKind::timeout,
                        .delivery = DeliveryStatus::not_started,
                        .exit_code = 0,
                        .diagnostic = "the command deadline passed before it started"};
}

} // namespace

CommandChannel::CommandChannel(std::size_t operation_limit) noexcept
    : operation_limit_{operation_limit} {}

bool CommandChannel::admit() noexcept {
  std::lock_guard lock{mutex_};
  if (in_flight_ >= operation_limit_) {
    return false;
  }
  ++in_flight_;
  return true;
}

void CommandChannel::release() noexcept {
  std::lock_guard lock{mutex_};
  if (in_flight_ > 0U) {
    --in_flight_;
  }
}

expected<std::shared_ptr<CommandEngine>, CommandFailure>
CommandEngine::start(CommandEngineConfig config) {
  if (config.worker_count == 0U) {
    return unexpected(
        CommandFailure{.kind = FailureKind::validation,
                       .delivery = DeliveryStatus::not_started,
                       .exit_code = 0,
                       .diagnostic = "the command engine needs at least one worker"});
  }
  auto channel = std::make_shared<CommandChannel>(config.operation_limit);
  std::shared_ptr<CommandEngine> engine{new CommandEngine{std::move(channel)}};
  engine->workers_.reserve(config.worker_count);
  for (std::size_t index = 0; index < config.worker_count; ++index) {
    engine->workers_.emplace_back([owner = engine.get()] { owner->worker_loop(); });
  }
  return engine;
}

CommandEngine::CommandEngine(std::shared_ptr<CommandChannel> channel) noexcept
    : channel_{std::move(channel)} {}

CommandEngine::~CommandEngine() { close(); }

Operation<std::string> CommandEngine::submit(
    std::shared_ptr<const SubprocessBackend> backend, CommandRequest command,
    std::optional<std::chrono::milliseconds> timeout,
    std::optional<std::size_t> output_limit, MoveOnlyFunction<void()> retirement_hook) {
  if (!channel_->admit()) {
    auto refused = make_operation<std::string>(std::make_shared<UnadmittedHooks>());
    static_cast<void>(refused.source.publish(unexpected(CommandFailure{
        .kind = FailureKind::overloaded,
        .delivery = DeliveryStatus::not_started,
        .exit_code = 0,
        .diagnostic = "the command engine has more work in flight than it accepts"})));
    retire(refused.source, retirement_hook);
    return std::move(refused.operation);
  }

  auto started = make_operation<std::string>(std::make_shared<ChannelHooks>(channel_));
  std::optional<Clock::time_point> deadline;
  if (timeout.has_value()) {
    deadline = Clock::now() + *timeout;
  }
  {
    std::lock_guard lock{mutex_};
    if (!stop_requested_) {
      started.source.mark_dispatching();
      pending_.push_back(PendingCommand{.backend = std::move(backend),
                                        .command = std::move(command),
                                        .timeout = timeout,
                                        .output_limit = output_limit,
                                        .source = std::move(started.source),
                                        .deadline = deadline,
                                        .retirement_hook = std::move(retirement_hook)});
      ready_.notify_one();
      return std::move(started.operation);
    }
  }
  static_cast<void>(started.source.publish(
      unexpected(cancelled(DeliveryStatus::not_started,
                           "the command engine closed before this was accepted"))));
  retire(started.source, retirement_hook);
  return std::move(started.operation);
}

void CommandEngine::worker_loop() {
  for (;;) {
    PendingCommand work;
    {
      std::unique_lock lock{mutex_};
      ready_.wait(lock, [this] { return stop_requested_ || !pending_.empty(); });
      if (pending_.empty()) {
        return;
      }
      work = std::move(pending_.front());
      pending_.pop_front();
    }

    const bool withdrawn = work.source.cancel_requested();
    bool stopping = false;
    {
      std::lock_guard lock{mutex_};
      stopping = stop_requested_;
    }
    if (withdrawn || stopping) {
      static_cast<void>(work.source.publish(unexpected(
          cancelled(DeliveryStatus::not_started,
                    withdrawn ? "the caller withdrew the command before it started"
                              : "the command engine closed before this started"))));
      retire(work.source, work.retirement_hook);
      continue;
    }
    if (work.deadline.has_value() && Clock::now() >= *work.deadline) {
      static_cast<void>(work.source.publish(unexpected(timed_out())));
      retire(work.source, work.retirement_hook);
      continue;
    }

    if (work.deadline.has_value()) {
      work.timeout =
          std::chrono::ceil<std::chrono::milliseconds>(*work.deadline - Clock::now());
    }
    work.source.mark_active();
    expected<std::string, CommandFailure> answer = unexpected(CommandFailure{
        .kind = FailureKind::pipe,
        .delivery = DeliveryStatus::indeterminate,
        .exit_code = 0,
        .diagnostic = "the command worker could not finish this command"});
    try {
#if defined(_WIN32)
      answer = work.backend->run_cancellable_unobserved(
          work.command, work.timeout, work.output_limit,
          [&work] { return work.source.cancel_requested(); });
#else
      answer =
          work.backend->run_unobserved(work.command, work.timeout, work.output_limit);
#endif
    } catch (...) {
    }
    try {
      static_cast<void>(work.source.publish(std::move(answer)));
    } catch (...) {
    }
    work.source.begin_retirement();
    retire(work.source, work.retirement_hook);
  }
}

void CommandEngine::request_stop() noexcept {
  {
    std::lock_guard lock{mutex_};
    stop_requested_ = true;
  }
  ready_.notify_all();
}

void CommandEngine::close() {
  {
    std::unique_lock lock{mutex_};
    if (terminal_) {
      return;
    }
    if (close_joining_) {
      terminal_ready_.wait(lock, [this] { return terminal_; });
      return;
    }
    stop_requested_ = true;
    close_joining_ = true;
  }
  ready_.notify_all();
  for (auto& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  {
    std::lock_guard lock{mutex_};
    terminal_ = true;
  }
  terminal_ready_.notify_all();
}

} // namespace detail
LIBTMUX_NAMESPACE_END
