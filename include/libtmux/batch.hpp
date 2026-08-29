#pragma once

// Build one tmux command sequence from several commands.
//
// tmux accepts multiple commands in a single invocation separated by a `;`
// argument. Because the core execs argv directly and never goes through a
// shell, the separator is a bare `;` element — there is no backslash to escape
// and no quoting to get wrong.
//
// A batch is one fail-fast group: tmux stops at the first command that errors.
// That is why a batch is a distinct type from a list of independent requests,
// which the transport runs separately and attributes individually.

#include "libtmux/abi.hpp"
#include "libtmux/command.hpp"
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

LIBTMUX_NAMESPACE_BEGIN

inline constexpr std::string_view kCommandSeparator = ";";

class CommandBatch {
public:
  // Append one command. An empty command is rejected rather than emitted,
  // because an empty argv between separators makes tmux read the next
  // command's name as an argument.
  bool add(CommandRequest command) {
    if (command.empty()) {
      return false;
    }
    commands_.push_back(std::move(command));
    return true;
  }

  [[nodiscard]] std::size_t size() const noexcept { return commands_.size(); }
  [[nodiscard]] bool empty() const noexcept { return commands_.empty(); }

  // Render the whole batch as one argv. A single command renders with no
  // separator, so a batch of one is byte-identical to running it alone.
  [[nodiscard]] CommandRequest request() const {
    CommandRequest result;
    for (const CommandRequest& command : commands_) {
      if (!result.empty()) {
        result.emplace_back(kCommandSeparator);
      }
      for (const CommandArgument& argument : command.arguments()) {
        result.push_back(argument);
      }
    }
    return result;
  }

  [[nodiscard]] std::vector<std::string> argv() const { return request().argv(); }

  [[nodiscard]] const std::vector<CommandRequest>& commands() const noexcept {
    return commands_;
  }

private:
  std::vector<CommandRequest> commands_;
};

LIBTMUX_NAMESPACE_END
