#include "support_commands.hpp"

#include <array>
#include <cstddef>

LIBTMUX_NAMESPACE_BEGIN
namespace support::commands {

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

} // namespace support::commands
LIBTMUX_NAMESPACE_END
