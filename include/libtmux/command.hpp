#pragma once

// Why a tmux command produced no answer.
//
// `refused` means tmux ran and said no; `missing` means tmux ran, said yes,
// and the object asked about was not there; `truncated` means it answered at
// greater length than the caller allowed for. Every other value means tmux never
// got that far. They stay apart because the caller's next move differs — a
// rejected argument is a bug, a spawn failure is an environment problem, a
// timeout may be worth retrying, and a missing object is ordinary in a program
// that races a user closing a pane.

#include "libtmux/abi.hpp"
#include <functional>
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

LIBTMUX_NAMESPACE_END
