#pragma once

// Why a tmux command produced no answer.
//
// `refused` means tmux ran and said no; `missing` means tmux ran, said yes,
// and the object asked about was not there; `truncated` means it answered at
// greater length than the caller allowed for. `unsupported` is a backend
// feature gap; `validation` is a bad request, so callers handle them differently.

#include "libtmux/abi.hpp"
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

LIBTMUX_NAMESPACE_BEGIN

enum class FailureKind {
  validation,
  spawn,
  pre_exec,
  pipe,
  timeout,
  refused,
  missing,
  // tmux ran and answered, and the answer did not fit. Reported rather than
  // returned, because a truncated answer is indistinguishable from a complete
  // one: the last line is simply cut, mid-word.
  truncated,
  // The backend cannot provide this operation without weakening its contract;
  // nothing was dispatched.
  unsupported,
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
  }
  return "unknown failure";
}

struct CommandFailure {
  FailureKind kind{FailureKind::refused};
  // True only when tmux itself ran. Retrying a dispatched command repeats
  // whatever it already did.
  bool dispatched{};
  int exit_code{};
  std::string diagnostic;
};

// Told about every command, as it finishes.
//
// There is otherwise no way to see what this library ran: a caller debugging a
// tmux interaction has only the failures, and nothing at all when things
// succeed. The command is rendered as tmux received it, with any argument
// marked sensitive replaced.
//
// Called on the thread that ran the command, while nothing is held, so an
// observer that itself calls tmux does not deadlock — but one shared between
// threads has to say so itself.
using CommandObserver =
    std::function<void(std::string_view command, const CommandFailure* failure)>;

// What a call waits and holds when the caller did not say.
//
// A timeout on the call is still how a caller says "this one in particular":
// listing sessions does not share a deadline with attaching a client. What was
// missing was a floor. Typed methods passed no timeout at all, so
// `window.rename(...)` waited for as long as the process ran if tmux never
// answered — and "tmux is normally fast" is not a liveness guarantee when a
// hook blocks, a filesystem stops answering, or a connection breaks without
// closing the pipe.
//
// Thirty seconds is far past every tmux command that works and far short of
// forever. `wait_for` opts out, because waiting is the whole request.
struct ExecutionPolicy {
  // Absent means wait. That is a thing to mean deliberately.
  std::optional<std::chrono::milliseconds> timeout{std::chrono::seconds{30}};
  // Absent leaves the transport's own bound, which is one megabyte.
  std::optional<std::size_t> output_limit{};
};

LIBTMUX_NAMESPACE_END
