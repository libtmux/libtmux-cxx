#pragma once

// How tmux reads a command line: where one command ends, which command a word
// names, and which flags take a value.
//
// A raw command reaches the daemon through the same parser as a typed one, so a
// guard that reads only the spellings this library writes misses every other
// spelling tmux accepts. These functions read a command the way the daemon
// does, and the guards built on them agree with it.

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtmux/abi.hpp"
#include "libtmux/command.hpp"
#include "libtmux/version.hpp"

LIBTMUX_NAMESPACE_BEGIN
namespace support::commands {

/// The command a word names, as tmux's `cmd_find` resolves it: an exact name,
/// an alias, or a prefix of exactly one name. Empty for an unknown or an
/// ambiguous word, which tmux rejects with the whole list.
[[nodiscard]] std::optional<std::string_view> resolve(std::string_view word);

/// One command of a list.
struct Invocation {
  /// The full command name, whatever spelling the argv used.
  std::string_view name;
  /// Everything after the command word.
  std::vector<std::string> arguments;
};

/// The commands an argv holds, separated the way tmux separates them: at an
/// element that is `;`, and after an element that ends in an unescaped `;`.
/// Commands whose first word does not resolve are left out.
[[nodiscard]] std::vector<Invocation> invocations(const std::vector<std::string>& argv);

/// The flags and operands of one command, split as tmux's `args_parse` does:
/// flags stop at the first operand or at `--`, and a flag that takes a value
/// reads the rest of its cluster or else the next element.
struct Flags {
  struct Set {
    char flag;
    std::string value;
  };
  std::vector<Set> set;
  std::vector<std::string> operands;

  [[nodiscard]] bool has(char flag) const noexcept;
  /// The value of the last occurrence; absent when the flag is not set.
  [[nodiscard]] std::optional<std::string_view> value(char flag) const noexcept;
};

/// `valued` lists the flags of the command that take a value.
[[nodiscard]] Flags parse_flags(std::span<const std::string> arguments,
                                std::string_view valued);

/// The refusal for an operation that a daemon older than `needed_text` cannot do
/// safely. The typed calls and the literal check word it the same way.
[[nodiscard]] CommandFailure older_daemon(std::string_view operation,
                                          std::string_view needed_text);

/// The refusal for a session or window name that tmux 3.7 would reject after
/// the command had partly run. `what` is "session" or "window".
[[nodiscard]] CommandFailure separator_in_name(std::string_view what);

/// Why a literal command list must not be sent to a daemon running `running`,
/// or nothing when every command in it may be.
///
/// This is the check the typed calls get from their own gates, applied to what
/// a caller writes by hand. Each command is read as tmux reads it, under its
/// alias or any unique prefix, and each is held to every gate the registry
/// lists it under. `live` says the version came from a daemon rather than from
/// the executable that would start one.
[[nodiscard]] std::optional<CommandFailure>
refuse_literal(const std::vector<std::string>& argv, const Version& running, bool live);

} // namespace support::commands
LIBTMUX_NAMESPACE_END
