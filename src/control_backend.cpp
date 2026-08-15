#include "control_backend.hpp"

#include <cstddef>
#include <utility>

LIBTMUX_NAMESPACE_BEGIN
namespace detail {

namespace {

std::string text(const std::vector<std::byte>& bytes) {
  std::string out;
  out.reserve(bytes.size());
  for (const std::byte byte : bytes) {
    out.push_back(static_cast<char>(byte));
  }
  return out;
}

CommandFailure carried(FailureKind kind, bool dispatched, std::string diagnostic) {
  return CommandFailure{.kind = kind,
                        .dispatched = dispatched,
                        .exit_code = 0,
                        .diagnostic = std::move(diagnostic)};
}

} // namespace

ControlBackend::ControlBackend(Connection connection, std::vector<std::string> selector,
                               CommandObserver observer)
    : Backend{std::move(observer)}, connection_{std::move(connection)},
      selector_{std::move(selector)} {}

expected<std::shared_ptr<const ControlBackend>, ProtocolError>
ControlBackend::open(std::vector<std::string> selector, std::string socket_path,
                     std::string session, CommandObserver observer) {
  ConnectionOptions options;
  options.socket_path = std::move(socket_path);
  options.session_name = std::move(session);
  auto connection = Connection::connect(std::move(options));
  if (!connection.has_value()) {
    return unexpected(connection.error());
  }
  return std::make_shared<const ControlBackend>(
      *std::move(connection), std::move(selector), std::move(observer));
}

expected<std::string, CommandFailure>
ControlBackend::run(const std::vector<std::string>& command,
                    std::optional<std::chrono::milliseconds> timeout,
                    std::optional<std::size_t> output_limit) const {
  ControlRequest request;
  request.group.push_back(ControlCommand{command});

  // No timeout means no deadline. The furthest representable point is what
  // "wait as long as it takes" is, on a clock that only ever moves forward.
  const auto deadline = timeout.has_value()
                            ? std::chrono::steady_clock::now() + *timeout
                            : std::chrono::steady_clock::time_point::max();

  const std::lock_guard<std::mutex> held{mutex_};
  ControlRequestResult result = connection_.execute(std::move(request), deadline);

  const auto reported = [this, &command](CommandFailure failure) {
    observe(command, &failure);
    return unexpected(std::move(failure));
  };

  if (result.connection_error.has_value()) {
    // The connection is what failed, so whether tmux acted is unknowable from
    // here: reported as dispatched, which is the answer that does not invite
    // a retry of something that may already have happened.
    return reported(carried(FailureKind::pipe, true, result.connection_error->message));
  }
  if (result.operations.empty()) {
    // The transport misbehaved rather than tmux refusing, which is what the
    // pipe kind already means.
    return reported(
        carried(FailureKind::pipe, true, "the connection returned no reply block"));
  }

  const ControlOperationResult& operation = result.operations.front();
  if (operation.attribution != Attribution::exact || !operation.block.has_value()) {
    // A reply that cannot be attributed is the control-mode shape of a
    // timeout: the command reached tmux, and what it did is unknown.
    return reported(carried(FailureKind::timeout, true,
                            "the reply could not be matched to this command"));
  }

  std::string body = text(operation.block->body);
  // The same bound the subprocess transport applies, so the two agree about
  // what a caller asked for rather than differing by transport.
  if (output_limit.has_value() && body.size() > *output_limit) {
    return reported(carried(FailureKind::truncated, true,
                            "tmux produced more output than the " +
                                std::to_string(*output_limit) +
                                " byte limit this call allowed for"));
  }
  if (operation.block->terminal == ControlTerminal::error) {
    return reported(carried(FailureKind::refused, true, std::move(body)));
  }
  observe(command, nullptr);
  return body;
}

expected<std::string, CommandFailure>
ControlBackend::run_batch(const CommandBatch& batch,
                          std::optional<std::chrono::milliseconds> timeout,
                          std::optional<std::size_t> output_limit) const {
  // One operation per command, so the separator between them is the
  // protocol's. Flattened into a single argv the separator would be escaped
  // like any other byte and arrive as a literal semicolon, which tmux reads
  // as an argument to the first command — and then reports success.
  ControlRequest request;
  for (const std::vector<std::string>& command : batch.commands()) {
    request.group.push_back(ControlCommand{command});
  }
  const std::vector<std::string> observed = batch.argv();

  const auto deadline = timeout.has_value()
                            ? std::chrono::steady_clock::now() + *timeout
                            : std::chrono::steady_clock::time_point::max();

  const std::lock_guard<std::mutex> held{mutex_};
  ControlRequestResult result = connection_.execute(std::move(request), deadline);

  const auto reported = [this, &observed](CommandFailure failure) {
    observe(observed, &failure);
    return unexpected(std::move(failure));
  };

  if (result.connection_error.has_value()) {
    return reported(carried(FailureKind::pipe, true, result.connection_error->message));
  }
  if (result.operations.size() != batch.commands().size()) {
    // Fewer replies than commands is the shape the old flattening produced:
    // something ran, and what is unknown. Said rather than passed off as the
    // first reply.
    return reported(carried(
        FailureKind::pipe, true,
        "tmux replied to " + std::to_string(result.operations.size()) + " of " +
            std::to_string(batch.commands().size()) + " commands in the batch"));
  }

  std::string body;
  for (std::size_t index = 0; index < result.operations.size(); ++index) {
    const ControlOperationResult& operation = result.operations[index];
    if (operation.attribution != Attribution::exact || !operation.block.has_value()) {
      return reported(carried(FailureKind::timeout, true,
                              "the reply to command " + std::to_string(index + 1) +
                                  " could not be matched to it"));
    }
    std::string part = text(operation.block->body);
    if (operation.block->terminal == ControlTerminal::error) {
      // A batch is fail-fast, and control mode can say which member stopped
      // it where the subprocess transport cannot.
      return reported(carried(FailureKind::refused, true,
                              "command " + std::to_string(index + 1) + " of " +
                                  std::to_string(result.operations.size()) + ": " +
                                  std::move(part)));
    }
    body += part;
  }
  if (output_limit.has_value() && body.size() > *output_limit) {
    return reported(carried(FailureKind::truncated, true,
                            "tmux produced more output than the " +
                                std::to_string(*output_limit) +
                                " byte limit this call allowed for"));
  }
  observe(observed, nullptr);
  return body;
}

expected<Version, CommandFailure> ControlBackend::version() const {
  auto reported =
      run({"display-message", "-p", "#{version}"}, std::nullopt, std::nullopt);
  if (!reported.has_value()) {
    return unexpected(reported.error());
  }
  // The format reports the version alone, where `tmux -V` prefixes it.
  const auto parsed = parse_version("tmux " + *reported);
  if (!parsed.has_value()) {
    return unexpected(carried(FailureKind::refused, true,
                              "tmux reported the version as " + *reported));
  }
  return *parsed;
}

std::vector<Notification> ControlBackend::take_notifications() const {
  const std::lock_guard<std::mutex> held{mutex_};
  return connection_.take_notifications();
}

std::size_t ControlBackend::dropped_notifications() const noexcept {
  const std::lock_guard<std::mutex> held{mutex_};
  return connection_.dropped_notifications();
}

} // namespace detail
LIBTMUX_NAMESPACE_END
