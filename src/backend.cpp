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
  }
  return FailureKind::spawn;
}

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

SubprocessBackend::SubprocessBackend(std::vector<std::string> connection,
                                     CommandObserver observer, ExecutionPolicy policy)
    : Backend{std::move(observer), policy}, connection_{std::move(connection)},
      identity_{resolved_socket_path(connection_).value_or(std::string{})} {}

expected<std::string, CommandFailure>
SubprocessBackend::run(const CommandRequest& command,
                       std::optional<std::chrono::milliseconds> timeout,
                       std::optional<std::size_t> output_limit) const {
  return run_scoped(command, std::nullopt, timeout, output_limit);
}

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
          .dispatched = false,
          .exit_code = 0,
          .diagnostic =
              "psmux cannot preserve semicolons or newlines in typed arguments"};
      return reported(std::move(failure));
    }
  }
  if (session_id.empty() || session_name.empty()) {
    CommandFailure failure{.kind = FailureKind::validation,
                           .dispatched = false,
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
                           .dispatched = true,
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
                             .dispatched = false,
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

expected<std::string, CommandFailure>
SubprocessBackend::run_scoped(const CommandRequest& command,
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

  // `CommandObserver` is told about every command, and these two are the
  // commands most worth seeing: one where tmux never started, and one where
  // the answer was too big to keep. Returning without a word left exactly the
  // failures a caller is debugging out of the log they turned on to debug them.
  const auto reported = [this, &command](CommandFailure failure) {
    return report_failure(command, std::move(failure));
  };

  const auto reply = run_process(request);
  if (!reply.has_value()) {
    return reported(CommandFailure{.kind = kind_of(reply.error().kind),
                                   .dispatched = reply.error().dispatch_phase !=
                                                 DispatchPhase::not_dispatched,
                                   .exit_code = -1,
                                   .diagnostic = reply.error().diagnostic});
  }

  if (reply->output_truncated) {
    // The runner bounds what it will hold. Returning the prefix as a complete
    // answer is the one outcome a caller cannot detect: the last line is cut
    // mid-way and looks like data.
    return reported(CommandFailure{.kind = FailureKind::truncated,
                                   .dispatched = true,
                                   .exit_code = 0,
                                   .diagnostic = "tmux produced more output than the " +
                                                 std::to_string(request.capture_limit) +
                                                 " byte limit this call allowed for"});
  }

  std::string out = text(reply->stdout_bytes);
  // A signalled tmux is a failure with no exit code of its own.
  const auto* exited = std::get_if<Exited>(&reply->termination);
  if (exited == nullptr || exited->code != 0) {
    // tmux reports why it refused on stderr, so a diagnostic built from
    // stdout alone is empty for every ordinary failure.
    std::string diagnostic = text(reply->stderr_bytes);
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
                                   .dispatched = true,
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
                                     .dispatched = true,
                                     .exit_code = 0,
                                     .diagnostic = "tmux -V printed " + *output});
  }
  return *version;
}

} // namespace detail
LIBTMUX_NAMESPACE_END
