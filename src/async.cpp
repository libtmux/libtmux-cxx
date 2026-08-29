#include "libtmux/async.hpp"

#include "backend.hpp"
#include "process.hpp"
#if !defined(_WIN32)
#include "process_engine.hpp"
#endif

#include <optional>
#include <utility>

#include "libtmux/server.hpp"

LIBTMUX_NAMESPACE_BEGIN

// Either a command still running, or the answer to one that has finished. A
// backend with no engine answers at submission, and the caller cannot tell the
// difference except by how long `wait` takes.
struct CommandOperation::State final {
  std::shared_ptr<const detail::Backend> backend;
  CommandRequest command;
  std::size_t allowed_bytes{0U};
  std::optional<expected<std::string, CommandFailure>> answered;
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
  if (state->answered.has_value()) {
    return *std::move(state->answered);
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
#if !defined(_WIN32)
  if (state_ && state_->running.has_value()) {
    return state_->running->request_cancel();
  }
#endif
  return false;
}

expected<CommandOperation, CommandFailure>
Server::submit(CommandRequest command) const {
  auto state = std::make_unique<CommandOperation::State>();
  state->backend = backend_;
  state->command = std::move(command);
  const auto& policy = backend_->policy();
  state->allowed_bytes = policy.output_limit.value_or(detail::default_capture_limit);
#if !defined(_WIN32)
  const auto* subprocess =
      dynamic_cast<const detail::SubprocessBackend*>(backend_.get());
  if (subprocess != nullptr) {
    auto started =
        subprocess->start(state->command, policy.timeout, policy.output_limit);
    if (!started.has_value()) {
      return unexpected(std::move(started.error()));
    }
    state->running = std::move(*started);
    return CommandOperation{std::move(state)};
  }
#endif
  // A backend with no engine has nowhere to put the work, so it answers now
  // and the operation carries the answer rather than pretending to wait.
  state->answered = backend_->run(state->command, policy.timeout, policy.output_limit);
  return CommandOperation{std::move(state)};
}

LIBTMUX_NAMESPACE_END