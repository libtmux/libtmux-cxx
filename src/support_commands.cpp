#include "support_commands.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

#include "backend.hpp"
#include "support_capabilities.hpp"

LIBTMUX_NAMESPACE_BEGIN
namespace support::commands {

using capabilities::CapabilityId;
using capabilities::CommandUse;
using capabilities::WorkaroundId;

namespace {

struct Entry {
  std::string_view name;
  std::string_view alias;
};

// tmux's `cmd_table`, in its order: `cmd_find` walks it front to back, and the
// order decides which of two prefixes is found first. Commands that only newer
// releases have are included, because a prefix is ambiguous on a release only
// if another command shares it there.
constexpr std::array kTable{
    Entry{"attach-session", "attach"},
    Entry{"bind-key", "bind"},
    Entry{"break-pane", "breakp"},
    Entry{"capture-pane", "capturep"},
    Entry{"choose-buffer", ""},
    Entry{"choose-client", ""},
    Entry{"choose-tree", ""},
    Entry{"clear-history", "clearhist"},
    Entry{"clear-prompt-history", "clearphist"},
    Entry{"clock-mode", ""},
    Entry{"command-prompt", ""},
    Entry{"confirm-before", "confirm"},
    Entry{"copy-mode", ""},
    Entry{"customize-mode", ""},
    Entry{"delete-buffer", "deleteb"},
    Entry{"detach-client", "detach"},
    Entry{"display-menu", "menu"},
    Entry{"display-message", "display"},
    Entry{"display-popup", "popup"},
    Entry{"display-panes", "displayp"},
    Entry{"find-window", "findw"},
    Entry{"has-session", "has"},
    Entry{"if-shell", "if"},
    Entry{"join-pane", "joinp"},
    Entry{"kill-pane", "killp"},
    Entry{"kill-server", ""},
    Entry{"kill-session", ""},
    Entry{"kill-window", "killw"},
    Entry{"last-pane", "lastp"},
    Entry{"last-window", "last"},
    Entry{"link-window", "linkw"},
    Entry{"list-buffers", "lsb"},
    Entry{"list-clients", "lsc"},
    Entry{"list-commands", "lscm"},
    Entry{"list-keys", "lsk"},
    Entry{"list-panes", "lsp"},
    Entry{"list-sessions", "ls"},
    Entry{"list-windows", "lsw"},
    Entry{"load-buffer", "loadb"},
    Entry{"lock-client", "lockc"},
    Entry{"lock-server", "lock"},
    Entry{"lock-session", "locks"},
    Entry{"move-pane", "movep"},
    Entry{"move-window", "movew"},
    Entry{"new-pane", "newp"},
    Entry{"new-session", "new"},
    Entry{"new-window", "neww"},
    Entry{"next-layout", "nextl"},
    Entry{"next-window", "next"},
    Entry{"paste-buffer", "pasteb"},
    Entry{"pipe-pane", "pipep"},
    Entry{"previous-layout", "prevl"},
    Entry{"previous-window", "prev"},
    Entry{"refresh-client", "refresh"},
    Entry{"rename-session", "rename"},
    Entry{"rename-window", "renamew"},
    Entry{"resize-pane", "resizep"},
    Entry{"resize-window", "resizew"},
    Entry{"respawn-pane", "respawnp"},
    Entry{"respawn-window", "respawnw"},
    Entry{"rotate-window", "rotatew"},
    Entry{"run-shell", "run"},
    Entry{"save-buffer", "saveb"},
    Entry{"select-layout", "selectl"},
    Entry{"select-pane", "selectp"},
    Entry{"select-window", "selectw"},
    Entry{"send-keys", "send"},
    Entry{"send-prefix", ""},
    Entry{"server-access", ""},
    Entry{"set-buffer", "setb"},
    Entry{"set-environment", "setenv"},
    Entry{"set-hook", ""},
    Entry{"set-option", "set"},
    Entry{"set-window-option", "setw"},
    Entry{"show-buffer", "showb"},
    Entry{"show-environment", "showenv"},
    Entry{"show-hooks", ""},
    Entry{"show-messages", "showmsgs"},
    Entry{"show-options", "show"},
    Entry{"show-prompt-history", "showphist"},
    Entry{"show-window-options", "showw"},
    Entry{"source-file", "source"},
    Entry{"split-window", "splitw"},
    Entry{"start-server", "start"},
    Entry{"suspend-client", "suspendc"},
    Entry{"swap-pane", "swapp"},
    Entry{"swap-window", "swapw"},
    Entry{"switch-client", "switchc"},
    Entry{"switch-mode", ""},
    Entry{"unbind-key", "unbind"},
    Entry{"unlink-window", "unlinkw"},
    Entry{"wait-for", "wait"},
};

// The flags of each guarded command that take a value, from the `args` template
// in tmux's `cmd-*.c`. A flag listed here consumes the next element, so a name
// that follows `-n` is read as its value and not as the flag after it.
struct Valued {
  std::string_view command;
  std::string_view flags;
};

constexpr std::array kValued{
    Valued{"break-pane", "FnstxXyY"},    Valued{"delete-buffer", "b"},
    Valued{"new-session", "cefFnstxy"},  Valued{"new-window", "cenFt"},
    Valued{"refresh-client", "ABCFfrt"}, Valued{"rename-session", "t"},
    Valued{"rename-window", "t"},        Valued{"select-layout", "t"},
    Valued{"set-buffer", "btn"},
};

Flags flags_of(const Invocation& command) {
  for (const Valued& each : kValued) {
    if (each.command == command.name) {
      return parse_flags(command.arguments, each.flags);
    }
  }
  return parse_flags(command.arguments, {});
}

bool lists(std::span<const CommandUse> uses, std::string_view command) {
  return std::ranges::any_of(
      uses, [&](const CommandUse& use) { return use.command == command; });
}

std::optional<CommandFailure> named_buffer(const Invocation& command,
                                           const Flags& flags, const Version& running) {
  const auto& named = capabilities::capability(CapabilityId::buffer_delete_named);
  if (running >= named.since.version || !lists(named.commands, command.name)) {
    return std::nullopt;
  }
  // Before 3.4 a missing name makes both commands act on the top buffer.
  if (command.name == "delete-buffer" && flags.has('b')) {
    return older_daemon("deleting a named buffer", named.since.text);
  }
  if (command.name == "set-buffer" && flags.has('b') && flags.has('n')) {
    return older_daemon("renaming a named buffer", named.since.text);
  }
  return std::nullopt;
}

std::optional<CommandFailure>
clipboard_query(const Invocation& command, const Flags& flags, const Version& running) {
  const auto& query = capabilities::capability(CapabilityId::clipboard_query);
  if (running >= query.since.version || !lists(query.commands, command.name) ||
      !flags.has('l')) {
    return std::nullopt;
  }
  // Before 3.4 the query ends a server that has a client attached.
  return older_daemon("a clipboard query", query.since.text);
}

std::optional<CommandFailure> layout(const Invocation& command, const Flags& flags,
                                     const Version& running) {
  const auto& mirrored = capabilities::capability(CapabilityId::layout_mirrored_main);
  if (!lists(mirrored.commands, command.name) || flags.operands.size() != 1U) {
    return std::nullopt;
  }
  return detail::layout_refusal(running, flags.operands.front());
}

std::optional<CommandFailure> separators(const Invocation& command, const Flags& flags,
                                         const Version& running) {
  const auto& workaround =
      capabilities::workaround(WorkaroundId::name_separators_rejected);
  if (!capabilities::affects(workaround.key, running)) {
    return std::nullopt;
  }
  for (const CommandUse& use : workaround.commands) {
    if (use.command != command.name) {
      continue;
    }
    // The name is the value of a listed flag, or the operand of a rename.
    std::vector<std::pair<std::string_view, char>> names;
    for (const std::string_view flag : use.flags) {
      if (const auto value = flags.value(flag.back()); value.has_value()) {
        names.emplace_back(*value, flag.back());
      }
    }
    if (use.flags.empty() && flags.operands.size() == 1U) {
      names.emplace_back(flags.operands.front(), ' ');
    }
    for (const auto& [name, flag] : names) {
      if (name.find_first_of(".:") == std::string_view::npos) {
        continue;
      }
      const bool session = command.name == "rename-session" ||
                           (command.name == "new-session" && flag != 'n');
      return separator_in_name(session ? "session" : "window");
    }
  }
  return std::nullopt;
}

std::optional<CommandFailure> unnamed_break(const Invocation& command,
                                            const Flags& flags, const Version& running,
                                            bool live) {
  const auto& workaround =
      capabilities::workaround(WorkaroundId::break_pane_misnames_windows);
  if (!live || !capabilities::affects(workaround.key, running) ||
      !lists(workaround.commands, command.name) || flags.has('n')) {
    return std::nullopt;
  }
  return CommandFailure{
      .kind = FailureKind::validation,
      .delivery = DeliveryStatus::not_started,
      .exit_code = 0,
      .diagnostic =
          "tmux 3.7 crashes on a break-pane that names no window; use Pane::break_out"};
}

} // namespace

std::optional<std::string_view> resolve(std::string_view word) {
  const Entry* found = nullptr;
  bool ambiguous = false;
  for (const Entry& entry : kTable) {
    if (!entry.alias.empty() && entry.alias == word) {
      ambiguous = false;
      found = &entry;
      break;
    }
    if (!entry.name.starts_with(word)) {
      continue;
    }
    if (found != nullptr) {
      ambiguous = true;
    }
    found = &entry;
    if (entry.name == word) {
      break;
    }
  }
  if (found == nullptr || ambiguous) {
    return std::nullopt;
  }
  return found->name;
}

std::vector<Invocation> invocations(const std::vector<std::string>& argv) {
  std::vector<std::vector<std::string>> commands(1);
  for (const std::string& argument : argv) {
    std::string word = argument;
    bool ends = false;
    if (!word.empty() && word.back() == ';') {
      word.pop_back();
      if (!word.empty() && word.back() == '\\') {
        word.back() = ';';
      } else {
        ends = true;
      }
    }
    if (!ends || !word.empty()) {
      commands.back().push_back(std::move(word));
    }
    if (ends) {
      commands.emplace_back();
    }
  }
  std::vector<Invocation> result;
  for (auto& command : commands) {
    if (command.empty()) {
      continue;
    }
    const auto name = resolve(command.front());
    if (!name.has_value()) {
      continue;
    }
    command.erase(command.begin());
    result.push_back(Invocation{*name, std::move(command)});
  }
  return result;
}

bool Flags::has(char flag) const noexcept {
  for (const Set& each : set) {
    if (each.flag == flag) {
      return true;
    }
  }
  return false;
}

std::optional<std::string_view> Flags::value(char flag) const noexcept {
  std::optional<std::string_view> found;
  for (const Set& each : set) {
    if (each.flag == flag) {
      found = each.value;
    }
  }
  return found;
}

Flags parse_flags(std::span<const std::string> arguments, std::string_view valued) {
  Flags flags;
  std::size_t index = 0;
  while (index < arguments.size()) {
    const std::string_view argument = arguments[index];
    if (argument.size() < 2U || argument.front() != '-') {
      break;
    }
    ++index;
    if (argument == "--") {
      break;
    }
    for (std::size_t at = 1; at < argument.size(); ++at) {
      const char flag = argument[at];
      if (valued.find(flag) == std::string_view::npos) {
        flags.set.push_back(Flags::Set{flag, {}});
        continue;
      }
      std::string value{argument.substr(at + 1U)};
      if (value.empty() && index < arguments.size()) {
        value = arguments[index++];
      }
      flags.set.push_back(Flags::Set{flag, std::move(value)});
      break;
    }
  }
  for (; index < arguments.size(); ++index) {
    flags.operands.push_back(arguments[index]);
  }
  return flags;
}

CommandFailure older_daemon(std::string_view operation, std::string_view needed_text) {
  return CommandFailure{.kind = FailureKind::unsupported,
                        .delivery = DeliveryStatus::not_started,
                        .exit_code = 0,
                        .diagnostic = std::string{operation} + " needs tmux " +
                                      std::string{needed_text} +
                                      " or newer; the daemon is older"};
}

CommandFailure separator_in_name(std::string_view what) {
  return CommandFailure{.kind = FailureKind::validation,
                        .delivery = DeliveryStatus::not_started,
                        .exit_code = 0,
                        .diagnostic = "tmux 3.7 refuses a " + std::string{what} +
                                      " name containing ':' or '.'"};
}

std::optional<CommandFailure> refuse_literal(const std::vector<std::string>& argv,
                                             const Version& running, bool live) {
  for (const Invocation& command : invocations(argv)) {
    const Flags flags = flags_of(command);
    if (auto failure = named_buffer(command, flags, running)) {
      return failure;
    }
    if (auto failure = clipboard_query(command, flags, running)) {
      return failure;
    }
    if (auto failure = layout(command, flags, running)) {
      return failure;
    }
    if (auto failure = separators(command, flags, running)) {
      return failure;
    }
    if (auto failure = unnamed_break(command, flags, running, live)) {
      return failure;
    }
  }
  return std::nullopt;
}

} // namespace support::commands
LIBTMUX_NAMESPACE_END
