#include "backend.hpp"

#include "libtmux/batch.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string_view>
#include <variant>

#include "environment.hpp"
#include "process.hpp"
#include "psmux.hpp"
#include "socket_identity.hpp"

// The ABI macros, not `namespace libtmux::detail`: that spelling opens a
// namespace beside the inline ABI one. A member definition still resolves
// through its class, so the mistake only shows up on a free function, as a
// link error naming a symbol that is defined right here.
LIBTMUX_NAMESPACE_BEGIN
namespace detail {

namespace {

#if defined(_WIN32)
FailureKind kind_of(ProcessError::Kind kind) noexcept {
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
#endif

// tmux reads a trailing `;` on an argument as a command separator, so an
// argument that ends in one arrives truncated — and, in a batch, whatever
// followed it becomes the next command. A backslash escapes the separator,
// and any backslashes already in front of it have to be doubled or they
// escape the escape instead. The batch separator is the one argument that
// means it. The control transport quotes its arguments and needs none of
// this, which is why the two transports disagreed.
std::string escaped_argument(const std::string& argument) {
  if (argument == kCommandSeparator || argument.empty() || argument.back() != ';') {
    return argument;
  }
  // Exactly one backslash. tmux strips the one immediately before a trailing
  // separator and leaves every other backslash alone, so doubling them here
  // would deliver an extra one.
  std::string escaped = argument;
  escaped.insert(escaped.size() - 1U, 1U, '\\');
  return escaped;
}

std::string text(const std::vector<std::byte>& bytes) {
  std::string out;
  out.reserve(bytes.size());
  for (const std::byte byte : bytes) {
    out.push_back(static_cast<char>(byte));
  }
  return out;
}

std::vector<std::string_view> sensitive_parts(const CommandRequest& command) {
  std::vector<std::string_view> parts;
  for (const CommandArgument& argument : command.arguments()) {
    for (const std::string& part : argument.sensitive_parts()) {
      if (!part.empty() &&
          std::ranges::find(parts, std::string_view{part}) == parts.end()) {
        parts.push_back(part);
      }
    }
  }
  return parts;
}

std::string redacted_text(std::string_view text,
                          const std::vector<std::string_view>& parts) {
  std::string_view marker{"[REDACTED]"};
  if (std::ranges::any_of(parts, [marker](const std::string_view part) {
        return marker.find(part) != std::string_view::npos;
      })) {
    marker = {};
  }
  std::string result;
  result.reserve(text.size());
  std::size_t cursor = 0U;
  while (cursor < text.size()) {
    std::size_t match = std::string_view::npos;
    std::size_t length = 0U;
    for (const std::string_view part : parts) {
      const std::size_t candidate = text.find(part, cursor);
      if (candidate < match || (candidate == match && part.size() > length)) {
        match = candidate;
        length = part.size();
      }
    }
    if (match == std::string_view::npos) {
      result.append(text.substr(cursor));
      break;
    }
    result.append(text.substr(cursor, match - cursor));
    result.append(marker);
    cursor = match + length;
  }
  if (std::ranges::any_of(parts, [&result](const std::string_view part) {
        return result.find(part) != std::string::npos;
      })) {
    return std::string{marker};
  }
  return result;
}

#if defined(_WIN32)
std::optional<std::string> psmux_session(const std::vector<std::string>& connection,
                                         std::string_view session) {
  if (session.empty()) {
    return std::nullopt;
  }
  if (connection.empty()) {
    return std::string{session};
  }
  if (connection.size() == 2U && connection.front() == "-L") {
    return connection.back() + "__" + std::string{session};
  }
  return std::nullopt;
}

#endif

} // namespace

std::string rendered_command(const CommandRequest& command) {
  constexpr std::size_t maximum = 300U;
  const std::vector<std::string_view> parts = sensitive_parts(command);
  std::string rendered;
  for (const CommandArgument& argument : command.arguments()) {
    if (!rendered.empty()) {
      rendered.push_back(' ');
    }
    rendered += redacted_text(argument.value(), parts);
    if (rendered.size() > maximum) {
      rendered.resize(maximum);
      rendered += "...";
      break;
    }
  }
  return rendered;
}

void Backend::observe(const CommandRequest& command,
                      const CommandFailure* failure) const {
  if (!observer_) {
    return;
  }
  observer_(rendered_command(command), failure);
}

CommandFailure Backend::redact(CommandFailure failure,
                               const CommandRequest& command) const {
  failure.diagnostic = redacted_text(failure.diagnostic, sensitive_parts(command));
  return failure;
}

expected<std::string, CommandFailure>
Backend::report_failure(const CommandRequest& command, CommandFailure failure) const {
  failure = redact(std::move(failure), command);
  observe(command, &failure);
  return unexpected(std::move(failure));
}

expected<PreparedAttach, CommandFailure>
Backend::prepare_attach(std::string_view target) const {
#if defined(_WIN32)
  static_cast<void>(target);
  return unexpected(CommandFailure{
      .kind = FailureKind::unsupported,
      .delivery = DeliveryStatus::not_started,
      .exit_code = 0,
      .diagnostic =
          "psmux cannot bind an attach command to a captured session safely"});
#else
  auto route = socket_alias();
  if (route == nullptr) {
    return unexpected(CommandFailure{
        .kind = FailureKind::unsupported,
        .delivery = DeliveryStatus::not_started,
        .exit_code = 0,
        .diagnostic = "this backend cannot retain an exact attach route"});
  }
  std::vector<std::string> command{"tmux"};
  const auto& selector = connection();
  command.insert(command.end(), selector.begin(), selector.end());
  command.emplace_back("attach-session");
  command.emplace_back("-t");
  command.emplace_back(target);
  return PreparedAttach{.argv = std::move(command), .route = std::move(route)};
#endif
}

SubprocessBackend::SubprocessBackend(std::vector<std::string> connection,
                                     std::string socket_path, std::string identity,
                                     std::shared_ptr<const SocketAlias> socket_alias,
                                     bool socket_missing, CommandObserver observer,
                                     ExecutionPolicy policy)
    : Backend{std::move(observer), policy}, connection_{std::move(connection)},
      identity_{std::move(identity)}, socket_path_{std::move(socket_path)},
      socket_alias_{std::move(socket_alias)}, socket_missing_{socket_missing} {}

expected<std::shared_ptr<const SubprocessBackend>, CommandFailure>
SubprocessBackend::open(std::vector<std::string> connection, CommandObserver observer,
                        ExecutionPolicy policy) {
  auto endpoint = bind_socket_endpoint(connection);
  if (!endpoint.has_value()) {
    return unexpected(CommandFailure{.kind = FailureKind::pipe,
                                     .delivery = DeliveryStatus::not_started,
                                     .exit_code = 0,
                                     .diagnostic = std::move(endpoint.error())});
  }
  auto backend = std::shared_ptr<SubprocessBackend>{new SubprocessBackend{
      std::move(endpoint->connection), std::move(endpoint->socket_path),
      std::move(endpoint->identity), std::move(endpoint->alias), endpoint->missing,
      std::move(observer), policy}};
#if !defined(_WIN32)
  auto engine = shared_engine();
  if (!engine.has_value()) {
    return unexpected(
        CommandFailure{.kind = FailureKind::pipe,
                       .delivery = DeliveryStatus::not_started,
                       .exit_code = 0,
                       .diagnostic = std::move(engine.error().diagnostic)});
  }
  backend->engine_ = std::move(*engine);
#endif
  return std::shared_ptr<const SubprocessBackend>{std::move(backend)};
}

expected<PreparedAttach, CommandFailure>
SubprocessBackend::prepare_attach(std::string_view target) const {
  if (socket_missing_) {
    return unexpected(CommandFailure{
        .kind = FailureKind::missing,
        .delivery = DeliveryStatus::not_started,
        .exit_code = 0,
        .diagnostic =
            "this handle predates the socket; reopen it after the server starts"});
  }
  return Backend::prepare_attach(target);
}

expected<std::string, CommandFailure>
SubprocessBackend::run(const CommandRequest& command,
                       std::optional<std::chrono::milliseconds> timeout,
                       std::optional<std::size_t> output_limit) const {
  return run_scoped(command, std::nullopt, timeout, output_limit);
}

#if defined(_WIN32)
expected<std::string, CommandFailure> SubprocessBackend::run_cancellable(
    const CommandRequest& command, std::optional<std::chrono::milliseconds> timeout,
    std::optional<std::size_t> output_limit, const CancellationProbe& cancelled) const {
  const bool version_query =
      command.size() == 1U && command.arguments().front().value() == "-V";
  if (socket_missing_ && !version_query) {
    return report_failure(
        command,
        CommandFailure{
            .kind = FailureKind::missing,
            .delivery = DeliveryStatus::not_started,
            .exit_code = 0,
            .diagnostic =
                "this handle predates the socket; reopen it after the server starts"});
  }
  ProcessRequest request = build_request(command, std::nullopt, timeout, output_limit);
  const auto allowed_bytes = request.capture_limit;
  auto reply = run_process(request, cancelled);
  if (!reply.has_value()) {
    return report_failure(command,
                          CommandFailure{.kind = kind_of(reply.error().kind),
                                         .delivery = reply.error().delivery,
                                         .exit_code = -1,
                                         .diagnostic = reply.error().diagnostic});
  }
  return interpret(command, allowed_bytes, *std::move(reply));
}
#endif

expected<std::string, CommandFailure> SubprocessBackend::run_in_session(
    const CommandRequest& command, std::string_view session_id,
    std::string_view session_name, std::optional<std::chrono::milliseconds> timeout,
    std::optional<std::size_t> output_limit) const {
#if defined(_WIN32)
  const auto reported = [this, &command](CommandFailure failure) {
    return report_failure(command, std::move(failure));
  };
  for (const CommandArgument& argument : command.arguments()) {
    if (libtmux_psmux::unsafe_command_argument(argument.value())) {
      CommandFailure failure{
          .kind = FailureKind::validation,
          .delivery = DeliveryStatus::not_started,
          .exit_code = 0,
          .diagnostic =
              "psmux cannot preserve semicolons or newlines in typed arguments"};
      return reported(std::move(failure));
    }
  }
  if (session_id.empty() || session_name.empty()) {
    CommandFailure failure{.kind = FailureKind::validation,
                           .delivery = DeliveryStatus::not_started,
                           .exit_code = 0,
                           .diagnostic =
                               "psmux cannot route an entity without its session"};
    return reported(std::move(failure));
  }

  const auto started = std::chrono::steady_clock::now();
  auto belongs = session_belongs(session_id, session_name, timeout);
  if (!belongs.has_value()) {
    return unexpected(belongs.error());
  }
  if (!*belongs) {
    CommandFailure failure{.kind = FailureKind::missing,
                           .delivery = DeliveryStatus::replied,
                           .exit_code = 0,
                           .diagnostic =
                               "tmux has no session " + std::string{session_name}};
    return reported(std::move(failure));
  }
  if (timeout.has_value()) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    if (elapsed >= *timeout) {
      CommandFailure failure{.kind = FailureKind::timeout,
                             .delivery = DeliveryStatus::not_started,
                             .exit_code = 0,
                             .diagnostic = "psmux session validation exhausted the "
                                           "command deadline"};
      return reported(std::move(failure));
    }
    timeout = *timeout - elapsed;
  }
  return run_scoped(command, session_name, timeout, output_limit);
#else
  static_cast<void>(session_id);
  static_cast<void>(session_name);
  return run_scoped(command, std::nullopt, timeout, output_limit);
#endif
}

expected<bool, CommandFailure> SubprocessBackend::session_belongs(
    std::string_view session_id, std::string_view session_name,
    std::optional<std::chrono::milliseconds> timeout) const {
#if defined(_WIN32)
  auto reply = run_scoped({"display-message", "-p", "-t", ":", "#{session_id}"},
                          session_name, timeout, std::nullopt);
  if (!reply.has_value()) {
    if (reply.error().kind == FailureKind::missing ||
        (reply.error().kind == FailureKind::refused &&
         libtmux_psmux::missing_session(reply.error().diagnostic))) {
      return false;
    }
    return unexpected(reply.error());
  }
  while (!reply->empty() && (reply->back() == '\n' || reply->back() == '\r')) {
    reply->pop_back();
  }
  return *reply == session_id;
#else
  static_cast<void>(session_id);
  static_cast<void>(session_name);
  static_cast<void>(timeout);
  return true;
#endif
}

#if !defined(_WIN32)
expected<SubprocessBackend::Started, CommandFailure>
SubprocessBackend::start(const CommandRequest& command,
                         std::optional<std::chrono::milliseconds> timeout,
                         std::optional<std::size_t> output_limit) const {
  const bool version_query =
      command.size() == 1U && command.arguments().front().value() == "-V";
  if (socket_missing_ && !version_query) {
    auto refused = interpret_failure(
        command,
        CommandFailure{
            .kind = FailureKind::missing,
            .delivery = DeliveryStatus::not_started,
            .exit_code = 0,
            .diagnostic =
                "this handle predates the socket; reopen it after the server starts"});
    return unexpected(std::move(refused.error()));
  }
  ProcessRequest request = build_request(command, std::nullopt, timeout, output_limit);
  // Read before the request is handed over, because the engine takes it.
  const auto allowed_bytes = request.capture_limit;
  return Started{.running = engine_->submit(std::move(request)),
                 .allowed_bytes = allowed_bytes};
}
#endif

ProcessRequest
SubprocessBackend::build_request(const CommandRequest& command,
                                 std::optional<std::string_view> session,
                                 std::optional<std::chrono::milliseconds> timeout,
                                 std::optional<std::size_t> output_limit) const {
  ProcessRequest request;
  request.executable = "tmux";
  request.timeout = timeout;
#if defined(_WIN32)
  // Warm claiming reserializes the caller's cwd into psmux's line protocol.
  request.environment = libtmux_env::psmux_child_environment();
  if (session.has_value()) {
    if (auto target = psmux_session(connection_, *session); target.has_value()) {
      request.environment.emplace_back("PSMUX_TARGET_SESSION", std::move(*target));
    }
  }
#else
  static_cast<void>(session);
#endif
  if (output_limit.has_value()) {
    request.capture_limit = *output_limit;
  }
  request.arguments.reserve(connection_.size() + command.size() + 1U);
  // Force UTF-8. The field separator this library asks tmux to print between
  // format values is U+241E, and a tmux that has decided the terminal is not
  // UTF-8 replaces it with an underscore — at which point every row fails to
  // split and every listing on the server fails. The control transport has
  // always passed this flag; the subprocess one is the reason a caller in a C
  // locale sees a library that cannot read anything.
  request.arguments.push_back(Argument{"-u"});
  for (const std::string& argument : connection_) {
    request.arguments.push_back(Argument{argument});
  }
  for (const CommandArgument& argument : command.arguments()) {
    request.arguments.push_back(
        Argument{escaped_argument(argument.value()),
                 static_cast<Sensitivity>(argument.sensitivity())});
  }

  return request;
}

expected<std::string, CommandFailure>
SubprocessBackend::run_scoped(const CommandRequest& command,
                              std::optional<std::string_view> session,
                              std::optional<std::chrono::milliseconds> timeout,
                              std::optional<std::size_t> output_limit) const {
  const bool version_query =
      command.size() == 1U && command.arguments().front().value() == "-V";
  if (socket_missing_ && !version_query) {
    return report_failure(
        command,
        CommandFailure{
            .kind = FailureKind::missing,
            .delivery = DeliveryStatus::not_started,
            .exit_code = 0,
            .diagnostic =
                "this handle predates the socket; reopen it after the server starts"});
  }
  ProcessRequest request = build_request(command, session, timeout, output_limit);

  // `CommandObserver` is told about every command, and these two are the
  // commands most worth seeing: one where tmux never started, and one where
  // the answer was too big to keep. Returning without a word left exactly the
  // failures a caller is debugging out of the log they turned on to debug them.
  const auto reported = [this, &command](CommandFailure failure) {
    return report_failure(command, std::move(failure));
  };

  // Read before the request is handed over, because the engine takes it.
  const auto allowed_bytes = request.capture_limit;
#if defined(_WIN32)
  // The engine is POSIX until the completion-port path exists, so this stays
  // on the runner that blocks the calling thread.
  const auto reply = run_process(request);
  if (!reply.has_value()) {
    return reported(CommandFailure{.kind = kind_of(reply.error().kind),
                                   .delivery = reply.error().delivery,
                                   .exit_code = -1,
                                   .diagnostic = reply.error().diagnostic});
  }
#else
  const auto reply = sync_wait(engine_->submit(std::move(request)));
  if (!reply.has_value()) {
    return reported(reply.error());
  }
#endif

  return interpret(command, allowed_bytes, *std::move(reply));
}

expected<std::string, CommandFailure>
SubprocessBackend::interpret_failure(const CommandRequest& command,
                                     CommandFailure failure) const {
  return report_failure(command, std::move(failure));
}

expected<std::string, CommandFailure>
SubprocessBackend::interpret(const CommandRequest& command, std::size_t allowed_bytes,
                             ProcessReply reply) const {
  const auto reported = [this, &command](CommandFailure failure) {
    return report_failure(command, std::move(failure));
  };
  const ProcessReply* const reply_ptr = &reply;
  if (reply_ptr->output_truncated) {
    // The runner bounds what it will hold. Returning the prefix as a complete
    // answer is the one outcome a caller cannot detect: the last line is cut
    // mid-way and looks like data.
    return reported(CommandFailure{.kind = FailureKind::truncated,
                                   .delivery = DeliveryStatus::replied,
                                   .exit_code = 0,
                                   .diagnostic = "tmux produced more output than the " +
                                                 std::to_string(allowed_bytes) +
                                                 " byte limit this call allowed for"});
  }

  std::string out = text(reply_ptr->stdout_bytes);
  // A signalled tmux is a failure with no exit code of its own.
  const auto* exited = std::get_if<Exited>(&reply_ptr->termination);
  if (exited == nullptr || exited->code != 0) {
    // tmux reports why it refused on stderr, so a diagnostic built from
    // stdout alone is empty for every ordinary failure.
    std::string diagnostic = text(reply_ptr->stderr_bytes);
    if (diagnostic.empty()) {
      diagnostic = std::move(out);
    }
    // tmux ends its message with a newline, and a caller putting the
    // diagnostic into a field of its own does not want one.
    while (!diagnostic.empty() &&
           (diagnostic.back() == '\n' || diagnostic.back() == '\r')) {
      diagnostic.pop_back();
    }
    // And which command it was: on its own, "can't find session: work" leaves
    // the reader to work out where in their program it came from.
    diagnostic += " (running: " + rendered_command(command) + ")";
    return reported(CommandFailure{.kind = FailureKind::refused,
                                   .delivery = DeliveryStatus::replied,
                                   .exit_code = exited == nullptr ? -1 : exited->code,
                                   .diagnostic = std::move(diagnostic)});
  }
  observe(command, nullptr);
  return out;
}

expected<Version, CommandFailure> SubprocessBackend::version() const {
  auto output = run({"-V"}, policy().timeout, policy().output_limit);
  if (!output.has_value()) {
    return unexpected(output.error());
  }
  const auto version = parse_version(*output);
  if (!version.has_value()) {
    return unexpected(CommandFailure{.kind = FailureKind::refused,
                                     .delivery = DeliveryStatus::replied,
                                     .exit_code = 0,
                                     .diagnostic = "tmux -V printed " + *output});
  }
  return *version;
}

} // namespace detail
LIBTMUX_NAMESPACE_END
