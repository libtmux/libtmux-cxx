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

Operation<std::string>
CommandEngine::submit(std::shared_ptr<const Backend> backend, CommandRequest command,
                      std::optional<std::chrono::milliseconds> timeout,
                      std::optional<std::size_t> output_limit) {
  if (!channel_->admit()) {
    auto refused = make_operation<std::string>(std::make_shared<UnadmittedHooks>());
    static_cast<void>(refused.source.publish(unexpected(CommandFailure{
        .kind = FailureKind::overloaded,
        .delivery = DeliveryStatus::not_started,
        .exit_code = 0,
        .diagnostic = "the command engine has more work in flight than it accepts"})));
    refused.source.retire();
    return std::move(refused.operation);
  }

  auto started = make_operation<std::string>(std::make_shared<ChannelHooks>(channel_));
  std::optional<Clock::time_point> deadline;
  if (timeout.has_value()) {
    deadline = Clock::now() + *timeout;
  }
  {
    std::lock_guard lock{mutex_};
    if (!closing_) {
      started.source.mark_dispatching();
      pending_.push_back(PendingCommand{.backend = std::move(backend),
                                        .command = std::move(command),
                                        .timeout = timeout,
                                        .output_limit = output_limit,
                                        .source = std::move(started.source),
                                        .deadline = deadline});
      ready_.notify_one();
      return std::move(started.operation);
    }
  }
  static_cast<void>(started.source.publish(
      unexpected(cancelled(DeliveryStatus::not_started,
                           "the command engine closed before this was accepted"))));
  started.source.retire();
  return std::move(started.operation);
}

void CommandEngine::worker_loop() {
  for (;;) {
    PendingCommand work;
    {
      std::unique_lock lock{mutex_};
      ready_.wait(lock, [this] { return closing_ || !pending_.empty(); });
      if (pending_.empty()) {
        return;
      }
      work = std::move(pending_.front());
      pending_.pop_front();
    }

    const bool withdrawn = work.source.cancel_requested();
    if (withdrawn) {
      static_cast<void>(work.source.publish(
          unexpected(cancelled(DeliveryStatus::not_started,
                               "the caller withdrew the command before it started"))));
      work.source.retire();
      continue;
    }
    if (work.deadline.has_value() && Clock::now() >= *work.deadline) {
      static_cast<void>(work.source.publish(unexpected(timed_out())));
      work.source.retire();
      continue;
    }

    if (work.deadline.has_value()) {
      work.timeout =
          std::chrono::ceil<std::chrono::milliseconds>(*work.deadline - Clock::now());
    }
    work.source.mark_active();
#if defined(_WIN32)
    expected<std::string, CommandFailure> answer;
    if (const auto* subprocess =
            dynamic_cast<const SubprocessBackend*>(work.backend.get())) {
      answer = subprocess->run_cancellable(
          work.command, work.timeout, work.output_limit,
          [&work] { return work.source.cancel_requested(); });
    } else {
      answer = work.backend->run(work.command, work.timeout, work.output_limit);
    }
#else
    auto answer = work.backend->run(work.command, work.timeout, work.output_limit);
#endif
    static_cast<void>(work.source.publish(std::move(answer)));
    work.source.begin_retirement();
    work.source.retire();
  }
}

void CommandEngine::close() {
  {
    std::lock_guard lock{mutex_};
    if (closing_) {
      return;
    }
    closing_ = true;
  }
  ready_.notify_all();
  for (auto& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

expected<std::shared_ptr<CommandEngine>, CommandFailure> shared_command_engine() {
  static std::mutex guard;
  static std::weak_ptr<CommandEngine> held;
  std::lock_guard lock{guard};
  if (auto engine = held.lock()) {
    return engine;
  }
  auto started = CommandEngine::start();
  if (!started.has_value()) {
    return unexpected(started.error());
  }
  held = *started;
  return started;
}

} // namespace detail
LIBTMUX_NAMESPACE_END
