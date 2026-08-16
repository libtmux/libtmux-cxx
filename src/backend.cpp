#include "backend.hpp"

#include "libtmux/batch.hpp"

#include <cstddef>
#include <variant>

#include "process.hpp"
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

} // namespace

std::string rendered_command(const std::vector<std::string>& command) {
  constexpr std::size_t maximum = 300U;
  std::string rendered;
  for (const std::string& argument : command) {
    if (!rendered.empty()) {
      rendered.push_back(' ');
    }
    rendered += argument;
    if (rendered.size() > maximum) {
      rendered.resize(maximum);
      rendered += "...";
      break;
    }
  }
  return rendered;
}

void Backend::observe(const std::vector<std::string>& command,
                      const CommandFailure* failure) const {
  if (!observer_) {
    return;
  }
  observer_(rendered_command(command), failure);
}

SubprocessBackend::SubprocessBackend(std::vector<std::string> connection,
                                     CommandObserver observer, ExecutionPolicy policy)
    : Backend{std::move(observer), policy}, connection_{std::move(connection)},
      identity_{resolved_socket_path(connection_).value_or(std::string{})} {}

expected<std::string, CommandFailure>
SubprocessBackend::run(const std::vector<std::string>& command,
                       std::optional<std::chrono::milliseconds> timeout,
                       std::optional<std::size_t> output_limit) const {
  ProcessRequest request;
  request.executable = "tmux";
  request.timeout = timeout;
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
  for (const std::string& argument : command) {
    request.arguments.push_back(Argument{escaped_argument(argument)});
  }

  // `CommandObserver` is told about every command, and these two are the
  // commands most worth seeing: one where tmux never started, and one where
  // the answer was too big to keep. Returning without a word left exactly the
  // failures a caller is debugging out of the log they turned on to debug them.
  const auto reported = [this, &command](CommandFailure failure) {
    observe(command, &failure);
    return unexpected(std::move(failure));
  };

  const auto reply = run_posix(request);
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
    CommandFailure failure{.kind = FailureKind::refused,
                           .dispatched = true,
                           .exit_code = exited == nullptr ? -1 : exited->code,
                           .diagnostic = std::move(diagnostic)};
    observe(command, &failure);
    return unexpected(std::move(failure));
  }
  observe(command, nullptr);
  return out;
}

expected<Version, CommandFailure> SubprocessBackend::version() const {
  auto output = run({"-V"});
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
