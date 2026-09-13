#include "completion.hpp"

#include <algorithm>
#include <ostream>

#include <CLI/CLI.hpp>

namespace libtmux::workspace::cli {
namespace {
std::vector<const CLI::Option*> options(const CLI::App& node) {
  auto result = node.get_options(
      [](const CLI::Option* option) { return !option->get_group().empty(); });
  for (const auto* group : node.get_subcommands(
           [](const CLI::App* child) { return child->get_name().empty(); })) {
    const auto grouped = options(*group);
    result.insert(result.end(), grouped.begin(), grouped.end());
  }
  return result;
}

std::vector<std::string> aliases(const CLI::Option& option) {
  std::vector<std::string> result;
  for (const auto& name : option.get_snames())
    result.push_back("-" + name);
  for (const auto& name : option.get_lnames())
    result.push_back("--" + name);
  return result;
}

const CLI::Option* lookup(const CLI::App& node, std::string_view value) {
  for (const auto* option : options(node))
    for (const auto& name : aliases(*option))
      if (name == value)
        return option;
  return nullptr;
}

bool file_option(const CLI::Option& option, const CLI::App& node) {
  for (const auto& name : aliases(option))
    if (name == "--save-to" || name == "--log-file" || name == "-S" ||
        (name == "-f" && node.get_name() == "load"))
      return true;
  return false;
}
} // namespace

void complete(const CLI::App& root, const CompletionChoices& choices,
              std::span<const std::string> words, std::ostream& output) {
  const CLI::App* node = &root;
  const CLI::Option* pending = nullptr;
  bool positional = false;
  for (std::size_t i = 0; i + 1 < words.size(); ++i) {
    const auto& word = words[i];
    if (pending != nullptr) {
      // Bash may expose an equals sign as its own completion word.
      if (word != "=")
        pending = nullptr;
      continue;
    }
    if (positional)
      continue;
    if (word == "--") {
      positional = true;
      continue;
    }
    if (word.starts_with("--")) {
      const auto equals = word.find('=');
      const auto* option = lookup(*node, std::string_view{word}.substr(0, equals));
      if (option != nullptr && option->get_expected_min() > 0 &&
          equals == std::string::npos)
        pending = option;
      continue;
    }
    if (word.size() > 1 && word.front() == '-') {
      for (std::size_t j = 1; j < word.size(); ++j) {
        const auto* option = lookup(*node, "-" + word.substr(j, 1));
        if (option != nullptr && option->get_expected_min() > 0) {
          if (j + 1 == word.size())
            pending = option;
          break;
        }
      }
      continue;
    }
    for (const auto* child : node->get_subcommands(
             [](const CLI::App* app) { return !app->get_name().empty(); })) {
      if (child->get_name() == word) {
        node = child;
        break;
      }
    }
  }

  std::string_view prefix = words.empty() ? std::string_view{} : words.back();
  std::string attached;
  if (!positional && pending == nullptr && prefix.starts_with("--")) {
    const auto equals = prefix.find('=');
    if (equals != std::string_view::npos) {
      pending = lookup(*node, prefix.substr(0, equals));
      if (pending != nullptr && pending->get_expected_min() > 0) {
        attached = prefix.substr(0, equals + 1);
        prefix.remove_prefix(equals + 1);
      } else {
        pending = nullptr;
      }
    }
  }
  if (!positional && pending == nullptr && prefix.size() > 2 && prefix.front() == '-' &&
      prefix[1] != '-') {
    for (std::size_t i = 1; i + 1 < prefix.size(); ++i) {
      const auto* option = lookup(*node, "-" + std::string{prefix.substr(i, 1)});
      if (option == nullptr)
        break;
      if (option->get_expected_min() > 0) {
        pending = option;
        attached = prefix.substr(0, i + 1);
        prefix.remove_prefix(i + 1);
        break;
      }
    }
  }
  if (pending != nullptr && prefix == "=")
    prefix = {};

  std::vector<std::string> candidates;
  bool files = false;
  if (pending != nullptr) {
    if (const auto values = choices.find(pending); values != choices.end())
      candidates = values->second;
    else
      files = file_option(*pending, *node);
  } else if (!positional && prefix.starts_with('-')) {
    for (const auto* option : options(*node)) {
      const auto names = aliases(*option);
      candidates.insert(candidates.end(), names.begin(), names.end());
    }
  } else if (!positional) {
    for (const auto* child : node->get_subcommands(
             [](const CLI::App* app) { return !app->get_name().empty(); }))
      candidates.push_back(child->get_name());
  }
  if (pending == nullptr && candidates.empty()) {
    for (const auto* option : options(*node))
      if (option->get_positional() && option->get_name() == "workspace-file")
        files = true;
  }
  output << (files ? "files\n" : "values\n");
  if (files && !attached.empty())
    output << attached << '\n';
  for (const auto& candidate : candidates)
    if (candidate.starts_with(prefix))
      output << attached << candidate << '\n';
}

std::string completion_script(std::string_view shell) {
  if (shell == "bash")
    return R"SH(_tmux_workspace_complete() {
  local -a reply=()
  local candidate prefix current
  COMPREPLY=()
  while IFS= read -r candidate; do
    reply+=("$candidate")
  done < <(command "${COMP_WORDS[0]}" --complete "${COMP_WORDS[@]:1:COMP_CWORD}" 2>/dev/null)
  if [[ ${reply[0]-} == files ]]; then
    prefix=${reply[1]-}
    if [[ -n $prefix ]]; then
      current=${COMP_WORDS[COMP_CWORD]#"$prefix"}
      for candidate in "$current"*; do
        [[ -e $candidate || -L $candidate ]] || continue
        [[ -d $candidate ]] && candidate+=/
        COMPREPLY+=("$prefix$candidate")
      done
      compopt -o filenames 2>/dev/null || :
    fi
    return
  fi
  compopt +o default 2>/dev/null || :
  for candidate in "${reply[@]:1}"; do
    COMPREPLY+=("$candidate")
  done
}
complete -o default -F _tmux_workspace_complete tmux-workspace
)SH";
  if (shell == "zsh")
    return R"SH(#compdef tmux-workspace
_tmux_workspace_complete() {
  local -a reply
  reply=("${(@f)$(command "${words[1]}" --complete "${words[@]:1:$((CURRENT-1))}" 2>/dev/null)}")
  if [[ ${reply[1]-} == files ]]; then
    if [[ -n ${reply[2]-} ]]; then
      compset -P "${reply[2]}"
    fi
    _files
  else
    compadd -- "${reply[@]:1}"
  fi
}
compdef _tmux_workspace_complete tmux-workspace
)SH";
  return R"SH(function __tmux_workspace_complete
  set -l words (commandline -opc)
  set -l reply (command "$words[1]" --complete $words[2..-1] (commandline -ct) 2>/dev/null)
  if test "$reply[1]" = files
    if test -n "$reply[2]"
      set -l current (commandline -ct)
      set -l start (math (string length -- "$reply[2]") + 1)
      for candidate in (__fish_complete_path (string sub --start $start -- "$current"))
        printf '%s%s\n' "$reply[2]" "$candidate"
      end
    else
      __fish_complete_path (commandline -ct)
    end
  else
    printf '%s\n' $reply[2..-1]
  end
end
complete -c tmux-workspace -f -a '(__tmux_workspace_complete)'
)SH";
}
} // namespace libtmux::workspace::cli
