#pragma once

#include "libtmux/server.hpp"
#include <functional>
#include <stdexec/execution.hpp>
#include <utility>

LIBTMUX_NAMESPACE_BEGIN
namespace detail {

struct CommandCancellationAccess final {
  static std::function<void()> get(const CommandOperation& operation) {
    return operation.cancellation_callback();
  }
};

struct ScheduledCommand final {
  Server server;
  CommandRuntime* runtime;
  CommandRequest request;

  template <class Token> expected<std::string, CommandFailure> operator()(Token token) {
    if (token.stop_requested()) {
      return unexpected(
          CommandFailure{.kind = FailureKind::cancelled,
                         .delivery = DeliveryStatus::not_started,
                         .diagnostic = "command cancelled before sender admission"});
    }
    auto operation = server.try_submit(*runtime, std::move(request));
    if (!operation) {
      return unexpected(std::move(operation.error()));
    }
    using StopCallback = stdexec::stop_callback_for_t<Token, std::function<void()>>;
    StopCallback cancellation{token, CommandCancellationAccess::get(*operation)};
    return std::move(*operation).wait();
  }
};

} // namespace detail

/// Submits lazily and waits on the supplied scheduler's worker.
/// Owns a Server copy. Runtime, scheduler and an explicit stop source must
/// outlive the connected operation. Each
/// pending command occupies one scheduler worker; dependent wait/signal
/// commands need enough workers to make progress. The value channel carries
/// expected<string, CommandFailure>, including cancellation and its delivery
/// information. The explicit stop token requests transport cancellation;
/// receiver stop requests may stop scheduling before command admission.
/// Other exceptions use the error channel. Global observers
/// remain subject to CommandRuntime::dispatch_ready.
template <stdexec::scheduler Scheduler>
[[nodiscard]] auto command_sender(Scheduler scheduler, const Server& server,
                                  CommandRuntime& runtime, CommandRequest request,
                                  stdexec::inplace_stop_token stop = {}) {
  return stdexec::then(stdexec::schedule(std::move(scheduler)),
                       [stop, task = detail::ScheduledCommand{
                                  server, &runtime, std::move(request)}]() mutable {
                         return task(stop);
                       });
}

LIBTMUX_NAMESPACE_END
