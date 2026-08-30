#include "libtmux/async.hpp"

#include "backend.hpp"
#include "command_engine.hpp"
#include "process.hpp"
#if !defined(_WIN32)
#include "process_engine.hpp"
#endif

#include <optional>
#include <utility>

#include "libtmux/server.hpp"

LIBTMUX_NAMESPACE_BEGIN

// The owning engine outlives accepted work even when its operation is dropped;
// dropping stops observation, not execution.
struct CommandOperation::State final {
  std::shared_ptr<const detail::Backend> backend;
  CommandRequest command;
  std::size_t allowed_bytes{0U};
  std::shared_ptr<detail::CommandEngine> command_engine;
  std::optional<detail::Operation<std::string>> command_running;
#if !defined(_WIN32)
  std::optional<detail::Operation<detail::ProcessReply>> running;
#endif
};

CommandOperation::CommandOperation(std::unique_ptr<State> state) noexcept
    : state_{std::move(state)} {}

CommandOperation::CommandOperation(CommandOperation&&) noexcept = default;
CommandOperation& CommandOperation::operator=(CommandOperation&&) noexcept = default;
CommandOperation::~CommandOperation() = default;

expected<std::string, CommandFailure> CommandOperation::wait() && {
  if (!state_) {
    return unexpected(
        CommandFailure{.kind = FailureKind::validation,
                       .delivery = DeliveryStatus::not_started,
                       .exit_code = 0,
                       .diagnostic = "this operation has been waited on"});
  }
  auto state = std::move(state_);
  if (state->command_running.has_value()) {
    return detail::sync_wait(*std::move(state->command_running));
  }
#if !defined(_WIN32)
  auto reply = detail::sync_wait(*std::move(state->running));
  const auto* subprocess =
      dynamic_cast<const detail::SubprocessBackend*>(state->backend.get());
  if (!reply.has_value()) {
    return subprocess == nullptr
               ? unexpected(reply.error())
               : subprocess->interpret_failure(state->command, reply.error());
  }
  if (subprocess == nullptr) {
    return unexpected(CommandFailure{.kind = FailureKind::unsupported,
                                     .delivery = DeliveryStatus::replied,
                                     .exit_code = 0,
                                     .diagnostic = "this backend cannot read a reply"});
  }
  return subprocess->interpret(state->command, state->allowed_bytes, *std::move(reply));
#else
  return unexpected(CommandFailure{.kind = FailureKind::unsupported,
                                   .delivery = DeliveryStatus::not_started,
                                   .exit_code = 0,
                                   .diagnostic = "no answer was recorded"});
#endif
}

bool CommandOperation::request_cancel() {
  if (state_ && state_->command_running.has_value()) {
    return state_->command_running->request_cancel();
  }
#if !defined(_WIN32)
  if (state_ && state_->running.has_value()) {
    return state_->running->request_cancel();
  }
#endif
  return false;
}

expected<CommandOperation, CommandFailure>
Server::submit(CommandRequest command, std::optional<std::chrono::milliseconds> timeout,
               std::optional<std::size_t> output_limit) const {
  // The policy fills in what the call did not say, exactly as `run` does.
  const ExecutionPolicy& policy = backend_->policy();
  const auto deadline = timeout.has_value() ? timeout : policy.timeout;
  const auto allowed = output_limit.has_value() ? output_limit : policy.output_limit;

  auto state = std::make_unique<CommandOperation::State>();
  state->backend = backend_;
  state->command = std::move(command);
#if !defined(_WIN32)
  const auto* subprocess =
      dynamic_cast<const detail::SubprocessBackend*>(backend_.get());
  if (subprocess != nullptr) {
    auto started = subprocess->start(state->command, deadline, allowed);
    if (!started.has_value()) {
      return unexpected(std::move(started.error()));
    }
    state->allowed_bytes = started->allowed_bytes;
    state->running = std::move(started->running);
    return CommandOperation{std::move(state)};
  }
#endif
  auto engine = detail::shared_command_engine();
  if (!engine.has_value()) {
    return unexpected(std::move(engine.error()));
  }
  state->command_engine = *engine;
  state->command_running =
      (*engine)->submit(backend_, state->command, deadline, allowed);
  return CommandOperation{std::move(state)};
}

LIBTMUX_NAMESPACE_END
