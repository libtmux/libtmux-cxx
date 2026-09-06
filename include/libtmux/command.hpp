#pragma once

// Why a tmux command produced no answer.
//
// `refused` means tmux ran and said no; `missing` means tmux ran, said yes,
// and the object asked about was not there; `truncated` means it answered at
// greater length than the caller allowed for. `unsupported` is a backend
// feature gap; `validation` is a bad request, so callers handle them
// differently.

#include "libtmux/abi.hpp"
#include "libtmux/delivery.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

LIBTMUX_NAMESPACE_BEGIN

/// Why a command did not produce an answer, in the order a caller would
/// diagnose them: bad before it left, then how it left, then what came back.
enum class FailureKind {
  validation,
  spawn,
  pre_exec,
  pipe,
  timeout,
  refused,
  missing,
  /// tmux ran and answered, and the answer did not fit. Reported rather than
  /// returned, because a truncated answer is indistinguishable from a complete
  /// one: the last line is simply cut, mid-word.
  truncated,
  /// The backend cannot provide this operation without weakening its contract;
  /// nothing was dispatched.
  unsupported,
  /// More work is in flight than the engine accepts. Nothing was dispatched, so
  /// this is the one refusal a caller may simply try again later — appended
  /// rather than grouped, because the values before it are an installed ABI.
  overloaded,
  /// The caller withdrew the call. Whether tmux acted is a separate question,
  /// and `delivery` is what answers it: a cancellation accepted before dispatch
  /// is not_started, and one accepted after the command was written is not.
  cancelled,
};

[[nodiscard]] constexpr std::string_view to_string(FailureKind kind) noexcept {
  switch (kind) {
  case FailureKind::validation:
    return "the request was rejected before tmux ran";
  case FailureKind::spawn:
    return "tmux could not be started";
  case FailureKind::pre_exec:
    return "the child failed before reaching tmux";
  case FailureKind::pipe:
    return "the transport failed";
  case FailureKind::timeout:
    return "tmux did not answer in time";
  case FailureKind::refused:
    return "tmux refused the command";
  case FailureKind::missing:
    return "tmux has no such object";
  case FailureKind::truncated:
    return "the answer did not fit";
  case FailureKind::unsupported:
    return "the backend does not support this operation";
  case FailureKind::overloaded:
    return "the engine has more work in flight than it accepts";
  case FailureKind::cancelled:
    return "the caller withdrew the command";
  }
  return "unknown failure";
}

/// A command that did not produce an answer, and how far it got.
///
/// `delivery` is the part that decides whether retrying is safe; `kind` and
/// `diagnostic` say what to fix.
struct CommandFailure {
  FailureKind kind{FailureKind::refused};
  /// How far the command is known to have progressed. Only `not_started` is
  /// safe to retry blindly.
  DeliveryStatus delivery{DeliveryStatus::not_started};
  int exit_code{};
  std::string diagnostic;
};

// One line naming what happened, what tmux said, and — through the delivery
// status — whether the call is safe to repeat.
[[nodiscard]] inline std::string to_string(const CommandFailure& failure) {
  std::string text{to_string(failure.kind)};
  if (!failure.diagnostic.empty()) {
    text += ": ";
    text += failure.diagnostic;
  }
  text += " (";
  // Only a command tmux answered has an exit status, and even then a child
  // that died from a signal has none: the transport writes -1 for the absence
  // of a status in both cases. The sign cannot stand in for that test, because
  // a Windows child reports its status through an int32_t and a crash is
  // legitimately negative.
  if (failure.delivery == DeliveryStatus::replied && failure.exit_code != 0 &&
      failure.exit_code != -1) {
    text += "exit ";
    text += std::to_string(failure.exit_code);
    text += ", ";
  }
  text += to_string(failure.delivery);
  text += ')';
  return text;
}

/// Whether an argument may appear in a diagnostic, a log, or an error message.
enum class ArgumentSensitivity : std::uint8_t { public_value, secret };

/// One argument, carrying whether any part of it is a secret.
///
/// Implicitly constructible from the string types so building a command reads
/// as a list of words; marking a part secret is the deliberate act.
class CommandArgument {
public:
  CommandArgument(const char* value) : value_{value} {}
  CommandArgument(std::string value) : value_{std::move(value)} {}
  CommandArgument(std::string_view value) : value_{value} {}

  [[nodiscard]] static CommandArgument sensitive(std::string value) {
    CommandArgument argument{std::move(value)};
    if (!argument.value_.empty()) {
      argument.sensitive_parts_.push_back(argument.value_);
    }
    return argument;
  }

  /// Keep a composite argument intact on the wire while treating one byte
  /// range inside it as sensitive. An invalid range hides the whole argument.
  [[nodiscard]] static CommandArgument
  sensitive_range(std::string value, std::size_t offset, std::size_t size) {
    CommandArgument argument{std::move(value)};
    if (offset > argument.value_.size() || size > argument.value_.size() - offset) {
      argument.sensitive_parts_.push_back(argument.value_);
      return argument;
    }
    if (size == 0U) {
      return argument;
    }
    argument.sensitive_parts_.push_back(argument.value_.substr(offset, size));
    return argument;
  }

  [[nodiscard]] const std::string& value() const noexcept { return value_; }
  [[nodiscard]] ArgumentSensitivity sensitivity() const noexcept {
    return sensitive_parts_.empty() ? ArgumentSensitivity::public_value
                                    : ArgumentSensitivity::secret;
  }
  [[nodiscard]] const std::vector<std::string>& sensitive_parts() const noexcept {
    return sensitive_parts_;
  }

private:
  std::string value_;
  std::vector<std::string> sensitive_parts_;
};

/// One tmux command as argv, with no shell between it and tmux.
///
/// Arguments are passed as separate words, so a value holding a space, a quote
/// or a `;` arrives whole and nothing here needs escaping.
class CommandRequest {
public:
  CommandRequest() = default;
  CommandRequest(std::initializer_list<CommandArgument> arguments)
      : arguments_{arguments} {}
  CommandRequest(std::vector<std::string> arguments) {
    arguments_.reserve(arguments.size());
    for (std::string& argument : arguments) {
      arguments_.emplace_back(std::move(argument));
    }
  }

  [[nodiscard]] bool empty() const noexcept { return arguments_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return arguments_.size(); }
  void reserve(std::size_t size) { arguments_.reserve(size); }

  void emplace_back(const char* value) { arguments_.emplace_back(value); }
  void emplace_back(std::string value) { arguments_.emplace_back(std::move(value)); }
  void emplace_back(std::string_view value) { arguments_.emplace_back(value); }
  void push_back(CommandArgument argument) {
    arguments_.push_back(std::move(argument));
  }

  [[nodiscard]] const std::vector<CommandArgument>& arguments() const noexcept {
    return arguments_;
  }
  [[nodiscard]] std::vector<std::string> argv() const {
    std::vector<std::string> result;
    result.reserve(arguments_.size());
    for (const CommandArgument& argument : arguments_) {
      result.push_back(argument.value());
    }
    return result;
  }

private:
  std::vector<CommandArgument> arguments_;
};

/// Told about every command, as it finishes.
///
/// There is otherwise no way to see what this library ran: a caller debugging a
/// tmux interaction has only the failures, and nothing at all when things
/// succeed. The command is rendered as tmux received it, with any argument
/// marked sensitive replaced.
///
/// Synchronous calls invoke it on their caller's thread. Asynchronous calls
/// invoke it only on the thread calling `CommandRuntime::dispatch_ready`.
/// No internal lock is held; a shared observer must synchronise itself.
struct CommandReport {
  /// As tmux received it, with any argument marked sensitive replaced. For a
  /// human reading a log.
  std::string_view command;
  /// The same command unrendered, for a caller building structured telemetry
  /// rather than a line of text. Empty when the report has no argv to give.
  std::span<const std::string> argv;
  /// Nothing when the command succeeded.
  const CommandFailure* failure;
  /// How long the command took, measured around its dispatch. Absent when
  /// nothing was dispatched — a request rejected before it ran took no time,
  /// and reporting zero would read as an immeasurably fast command.
  std::optional<std::chrono::nanoseconds> elapsed;
};

/// Every member expires on return; a caller keeping any of it copies it.
using CommandObserver = std::function<void(const CommandReport&)>;

/// What a call waits and holds when the caller did not say.
///
/// A timeout on the call is still how a caller says "this one in particular":
/// listing sessions does not share a deadline with attaching a client. What was
/// missing was a floor. Typed methods passed no timeout at all, so
/// `window.rename(...)` waited for as long as the process ran if tmux never
/// answered — and "tmux is normally fast" is not a liveness guarantee when a
/// hook blocks, a filesystem stops answering, or a connection breaks without
/// closing the pipe.
///
/// Thirty seconds is far past every tmux command that works and far short of
/// forever. `wait_for` opts out, because waiting is the whole request.
struct ExecutionPolicy {
  /// Absent means wait. That is a thing to mean deliberately.
  std::optional<std::chrono::milliseconds> timeout{std::chrono::seconds{30}};
  /// Absent leaves the transport's own bound, which is one megabyte.
  std::optional<std::size_t> output_limit{};
  // Which tmux to run. A bare name is resolved through `PATH`, as tmux's own
  // documentation assumes; a path containing a separator is used as given.
  //
  // Naming it is how a caller stops `PATH` deciding: a hermetic build, a
  // pinned version under test, or a wrapper that reaches tmux on another
  // machine. It rides the policy rather than the call because a Server's
  // connection is immutable, and because a handle that changed which tmux it
  // meant between two calls would make its own entities disagree.
  //
  // `Server::control` passes this to the connection it opens, so both
  // transports run the same executable unless the caller overrides it in
  // `ConnectionOptions`.
  std::filesystem::path tmux_binary{"tmux"};
};

// A transport a caller supplies.
//
// `BackendKind::custom` named this possibility from the first release, but
// nothing implemented it: the interface a backend had to satisfy lived in the
// library's private headers, so the only reachable transport was the one that
// launches a subprocess per command. This is the seam that makes the name
// true.
//
// One method, deliberately. Everything else a backend does — routing an entity
// command through its owning psmux session, proving a session belongs,
// preparing an attach argv — is either psmux's problem or the library's, and
// freezing it here would make a private arrangement permanent. What a
// transport owes is an answer to one command; the library supplies the rest
// and asks this for the tmux version too, by running `-V` through it.
//
// `run` is const and may be called from any thread, because a `Server` is
// copyable across threads and shares one executor. An implementation that
// keeps a connection or a buffer synchronises itself.
//
// Returning the command's standard output is the whole contract: a listing
// answers its rows, a mutation answers whatever tmux printed, and a failure
// answers `CommandFailure` rather than throwing.
//
// A batch arrives here too, as one request whose argv carries `;` between the
// grouped commands — there is no second method to implement, but the
// separators must reach tmux as they are. A transport that interprets or drops
// them turns one fail-fast group into something else without saying so.
class CommandExecutor {
public:
  CommandExecutor() = default;
  CommandExecutor(const CommandExecutor&) = delete;
  CommandExecutor& operator=(const CommandExecutor&) = delete;
  CommandExecutor(CommandExecutor&&) = delete;
  CommandExecutor& operator=(CommandExecutor&&) = delete;
  virtual ~CommandExecutor() = default;

  // Absent timeout means the caller named none; absent limit means the same.
  // An implementation that cannot bound itself should refuse rather than wait
  // forever, the way every transport here already does.
  [[nodiscard]] virtual expected<std::string, CommandFailure>
  run(const CommandRequest& command, std::optional<std::chrono::milliseconds> timeout,
      std::optional<std::size_t> output_limit) const = 0;
};

LIBTMUX_NAMESPACE_END

// Formatting a failure is how it reaches a log line, so the type every call
// can return knows how to write itself.
//
// Inheriting the string formatter keeps fill, alignment and width working, so
// `{:>40}` pads a failure exactly as it pads its text. `__cpp_lib_format` is
// deliberately not tested here: libc++ 18 leaves it undefined while
// `std::format` works, so guarding on it would drop this from the clang lane
// and keep it on the GCC one.
template <>
struct std::formatter<libtmux::CommandFailure> : std::formatter<std::string> {
  template <typename Context>
  auto format(const libtmux::CommandFailure& failure, Context& context) const {
    return std::formatter<std::string>::format(libtmux::to_string(failure), context);
  }
};
