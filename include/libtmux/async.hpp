#pragma once

// A command that has been sent and not yet answered.
//
// `Server::submit` returns one instead of blocking, so a program with several
// questions for tmux asks them all and then collects. The waiting is the
// caller's to do and to order; the engine underneath runs them at once on
// threads it owns, whatever number of them there are.
//
// An operation is answered once. Dropping one without waiting stops observing
// it; it does not stop the command, which tmux may already have run.

#include "libtmux/abi.hpp"

#include "libtmux/command.hpp"
#include "libtmux/expected.hpp"
#include <memory>
#include <string>

LIBTMUX_NAMESPACE_BEGIN

class Server;

class CommandOperation final {
public:
  CommandOperation(CommandOperation&& other) noexcept;
  CommandOperation& operator=(CommandOperation&& other) noexcept;
  CommandOperation(const CommandOperation&) = delete;
  CommandOperation& operator=(const CommandOperation&) = delete;
  ~CommandOperation();

  // What tmux said, once it has said it. The same answer `Server::run` gives,
  // including the bound on how much of it is kept and what a non-zero exit
  // means. Consumed once: the operation is spent afterwards.
  [[nodiscard]] expected<std::string, CommandFailure> wait() &&;

  // Ask for the command to be withdrawn. A request rather than an outcome:
  // tmux may answer first, and whether it acted is reported by the failure's
  // delivery rather than guessed at here.
  [[nodiscard]] bool request_cancel();

private:
  struct State;
  explicit CommandOperation(std::unique_ptr<State> state) noexcept;
  friend class Server;

  std::unique_ptr<State> state_;
};

LIBTMUX_NAMESPACE_END
