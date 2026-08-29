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

CommandFailure carried(FailureKind kind, DeliveryStatus delivery,
                       std::string diagnostic) {
  return CommandFailure{.kind = kind,
                        .delivery = delivery,
                        .exit_code = 0,
                        .diagnostic = std::move(diagnostic)};
}

expected<std::string, CommandFailure>
joined_reply(const std::vector<ControlBlock>& blocks,
             std::optional<std::size_t> output_limit) {
  if (blocks.empty()) {
    return unexpected(carried(FailureKind::pipe, DeliveryStatus::written,
                              "the connection returned no reply block"));
  }

  std::string body;
  for (const auto& block : blocks) {
    if (block.body_truncated) {
      return unexpected(
          carried(FailureKind::truncated, DeliveryStatus::replied,
                  "tmux produced " + std::to_string(block.body_bytes) +
                      " bytes in one reply, more than this connection retains"));
    }
    if (output_limit.has_value() && body.size() + block.body.size() > *output_limit) {
      return unexpected(carried(FailureKind::truncated, DeliveryStatus::replied,
                                "tmux produced more output than the " +
                                    std::to_string(*output_limit) +
                                    " byte limit this call allowed for"));
    }
    std::string part = text(block.body);
    if (block.terminal == ControlTerminal::error) {
      return unexpected(
          carried(FailureKind::refused, DeliveryStatus::replied, std::move(part)));
    }
    body += part;
  }
  return body;
}

} // namespace

ConnectionOptions routed_control_options(ConnectionOptions options,
                                         std::string socket_path, std::string session) {
  options.socket_path = std::move(socket_path);
  options.session_name = std::move(session);
  return options;
}

ControlRequest batch_request(const CommandBatch& batch) {
  // The protocol separator belongs between operations; flattening it into an
  // argv makes tmux read the remaining commands as arguments to the first.
  ControlRequest request;
  for (const CommandRequest& command : batch.commands()) {
    request.group.push_back(ControlCommand{command.argv()});
  }
  return request;
}

expected<std::string, CommandFailure>
inserted_command_reply(const ControlRequestResult& result,
                       std::optional<std::size_t> output_limit) {
  if (result.connection_error.has_value()) {
    return unexpected(carried(FailureKind::pipe, result.connection_error->delivery,
                              result.connection_error->message));
  }
  if (result.blocks.size() != 2U) {
    return unexpected(
        carried(FailureKind::pipe, DeliveryStatus::replied,
                "tmux returned " + std::to_string(result.blocks.size()) +
                    " reply blocks for a command and its inserted operation"));
  }

  const auto exact_block =
      [](const ControlBlock& block,
         std::string_view label) -> expected<const ControlBlock*, CommandFailure> {
    if (block.body_truncated) {
      return unexpected(carried(FailureKind::truncated, DeliveryStatus::replied,
                                std::string{label} + " produced " +
                                    std::to_string(block.body_bytes) +
                                    " bytes, more than this connection retains"));
    }
    return &block;
  };

  auto wrapper = exact_block(result.blocks[0], "if-shell");
  if (!wrapper.has_value()) {
    return unexpected(wrapper.error());
  }
  if ((*wrapper)->terminal == ControlTerminal::error) {
    return unexpected(
        carried(FailureKind::refused, DeliveryStatus::replied, text((*wrapper)->body)));
  }
  if (!(*wrapper)->body.empty() || (*wrapper)->body_bytes != 0U) {
    return unexpected(carried(FailureKind::pipe, DeliveryStatus::replied,
                              "if-shell returned output before its inserted command"));
  }

  auto inserted = exact_block(result.blocks[1], "inserted command");
  if (!inserted.has_value()) {
    return unexpected(inserted.error());
  }
  if (output_limit.has_value() && (*inserted)->body.size() > *output_limit) {
    return unexpected(carried(FailureKind::truncated, DeliveryStatus::replied,
                              "tmux produced more output than the " +
                                  std::to_string(*output_limit) +
                                  " byte limit this call allowed for"));
  }
  std::string body = text((*inserted)->body);
  if ((*inserted)->terminal == ControlTerminal::error) {
    return unexpected(
        carried(FailureKind::refused, DeliveryStatus::replied, std::move(body)));
  }
  return body;
}

ControlBackend::ControlBackend(Connection connection, std::vector<std::string> selector,
                               std::string socket_path, std::string identity,
                               CommandObserver observer, ExecutionPolicy policy,
                               std::shared_ptr<const SocketAlias> socket_alias)
    : Backend{std::move(observer), policy}, socket_alias_{std::move(socket_alias)},
      connection_{std::move(connection)}, selector_{std::move(selector)},
      socket_path_{std::move(socket_path)}, identity_{std::move(identity)} {}

expected<std::shared_ptr<const ControlBackend>, ProtocolError> ControlBackend::open(
    std::vector<std::string> selector, std::string socket_path, std::string identity,
    std::string session, ConnectionOptions options, CommandObserver observer,
    ExecutionPolicy policy, std::shared_ptr<const SocketAlias> socket_alias) {
  std::string retained_socket_path = socket_path;
  auto connection = Connection::connect(routed_control_options(
      std::move(options), std::move(socket_path), std::move(session)));
  if (!connection.has_value()) {
    return unexpected(connection.error());
  }
  return std::make_shared<const ControlBackend>(
      *std::move(connection), std::move(selector), std::move(retained_socket_path),
      std::move(identity), std::move(observer), policy, std::move(socket_alias));
}

expected<std::string, CommandFailure>
ControlBackend::run(const CommandRequest& command,
                    std::optional<std::chrono::milliseconds> timeout,
                    std::optional<std::size_t> output_limit) const {
  ControlRequest request;
  request.group.push_back(ControlCommand{command.argv()});

  const auto reported = [this, &command](CommandFailure failure) {
    return report_failure(command, std::move(failure));
  };
  // No timeout means no deadline. The furthest representable point is what
  // "wait as long as it takes" is, on a clock that only ever moves forward.
  const auto deadline = timeout.has_value()
                            ? std::chrono::steady_clock::now() + *timeout
                            : std::chrono::steady_clock::time_point::max();

  ControlRequestResult result = connection_.execute(std::move(request), deadline);

  if (result.connection_error.has_value()) {
    // Preserve the connection's exact progress. In particular, only a failure
    // before the writer started invites a blind retry.
    return reported(carried(FailureKind::pipe, result.connection_error->delivery,
                            result.connection_error->message));
  }
  auto reply = joined_reply(result.blocks, output_limit);
  if (!reply) {
    return reported(reply.error());
  }
  observe(command, nullptr);
  return reply;
}

expected<std::string, CommandFailure>
ControlBackend::run_inserted(const CommandRequest& command,
                             std::optional<std::chrono::milliseconds> timeout,
                             std::optional<std::size_t> output_limit) const {
  ControlRequest request;
  request.group.push_back(ControlCommand{command.argv()});
  const auto deadline = timeout.has_value()
                            ? std::chrono::steady_clock::now() + *timeout
                            : std::chrono::steady_clock::time_point::max();

  ControlRequestResult result = connection_.execute(std::move(request), deadline);

  auto reply = inserted_command_reply(result, output_limit);
  if (!reply.has_value()) {
    return report_failure(command, reply.error());
  }
  observe(command, nullptr);
  return reply;
}

expected<std::string, CommandFailure>
ControlBackend::run_batch(const CommandBatch& batch,
                          std::optional<std::chrono::milliseconds> timeout,
                          std::optional<std::size_t> output_limit) const {
  ControlRequest request = batch_request(batch);
  const CommandRequest observed = batch.request();

  const auto reported = [this, &observed](CommandFailure failure) {
    return report_failure(observed, std::move(failure));
  };
  const auto deadline = timeout.has_value()
                            ? std::chrono::steady_clock::now() + *timeout
                            : std::chrono::steady_clock::time_point::max();

  ControlRequestResult result = connection_.execute(std::move(request), deadline);

  if (result.connection_error.has_value()) {
    return reported(carried(FailureKind::pipe, result.connection_error->delivery,
                            result.connection_error->message));
  }
  auto reply = joined_reply(result.blocks, output_limit);
  if (!reply) {
    return reported(reply.error());
  }
  observe(observed, nullptr);
  return reply;
}

expected<Version, CommandFailure> ControlBackend::version() const {
  auto reported = run({"display-message", "-p", "#{version}"}, policy().timeout,
                      policy().output_limit);
  if (!reported.has_value()) {
    return unexpected(reported.error());
  }
  // The format reports the version alone, where `tmux -V` prefixes it.
  const auto parsed = parse_version("tmux " + *reported);
  if (!parsed.has_value()) {
    return unexpected(carried(FailureKind::refused, DeliveryStatus::replied,
                              "tmux reported the version as " + *reported));
  }
  return *parsed;
}

std::vector<Notification> ControlBackend::take_notifications() const {
  return connection_.take_notifications();
}

std::size_t ControlBackend::dropped_notifications() const noexcept {
  return connection_.dropped_notifications();
}

} // namespace detail
LIBTMUX_NAMESPACE_END
