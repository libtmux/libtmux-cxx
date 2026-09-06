#include "mcp_swap.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ranges>
#include <set>
#include <sstream>
#include <utility>

namespace libtmux::mcp_swap {
namespace {

std::string trim(std::string_view value) {
  const auto first = std::ranges::find_if(
      value, [](const unsigned char character) { return !std::isspace(character); });
  const auto last = std::ranges::find_if(
      value | std::views::reverse,
      [](const unsigned char character) { return !std::isspace(character); });
  if (first == value.end()) {
    return {};
  }
  return {first, last.base()};
}

std::string canonical_name(std::string name) {
  if (name == "antigravity") {
    return "agy";
  }
  return name;
}

std::string known_names(std::span<const Client> clients) {
  std::ostringstream result;
  for (std::size_t index = 0; index < clients.size(); ++index) {
    if (index != 0) {
      result << ", ";
    }
    result << clients[index].name;
  }
  return result.str();
}

Command parse_command(std::string_view value) {
  if (value == "detect") {
    return Command::detect;
  }
  if (value == "status") {
    return Command::status;
  }
  if (value == "use-local") {
    return Command::use_local;
  }
  if (value == "revert") {
    return Command::revert;
  }
  if (value == "doctor") {
    return Command::doctor;
  }
  if (value == "help" || value == "-h" || value == "--help") {
    return Command::help;
  }
  throw Error{'"' + std::string{value} + "\" is not a command"};
}

bool accepts(Command command, std::string_view flag) {
  if (flag == "--help") {
    return true;
  }
  switch (command) {
  case Command::detect:
  case Command::help:
    return false;
  case Command::status:
    return flag == "--repo" || flag == "--server" || flag == "--cli" ||
           flag == "--client" || flag == "--scope";
  case Command::use_local:
    return flag == "--repo" || flag == "--source" || flag == "--build-dir" ||
           flag == "--prefix" || flag == "--socket" || flag == "--no-preflight" ||
           flag == "--server" || flag == "--entry" || flag == "--env" ||
           flag == "--cli" || flag == "--client" || flag == "--scope" ||
           flag == "--dry-run";
  case Command::revert:
    return flag == "--cli" || flag == "--client" || flag == "--scope" ||
           flag == "--dry-run";
  case Command::doctor:
    return flag == "--repo" || flag == "--server";
  }
  return false;
}

bool takes_value(std::string_view flag) {
  return flag != "--help" && flag != "--dry-run" && flag != "--no-preflight";
}

std::pair<std::string, std::optional<std::string>>
split_flag(std::string_view argument) {
  const auto equals = argument.find('=');
  if (equals == std::string_view::npos) {
    return {std::string{argument}, std::nullopt};
  }
  return {std::string{argument.substr(0, equals)},
          std::string{argument.substr(equals + 1)}};
}

std::string require_value(std::span<const std::string> arguments, std::size_t& index,
                          std::string_view flag, std::optional<std::string> assigned) {
  if (!takes_value(flag)) {
    if (assigned.has_value()) {
      throw Error{std::string{flag} + " takes no value"};
    }
    return {};
  }
  if (!assigned.has_value()) {
    if (index + 1 >= arguments.size() || arguments[index + 1].starts_with('-')) {
      throw Error{std::string{flag} + " wants a value"};
    }
    assigned = arguments[++index];
  }
  if (assigned->empty()) {
    throw Error{std::string{flag} + " wants a nonempty value"};
  }
  return std::move(*assigned);
}

void require_path_component(std::string_view value, std::string_view flag) {
  if (value == "." || value == ".." || value.find('/') != std::string_view::npos ||
      value.find('\\') != std::string_view::npos) {
    throw Error{std::string{flag} + " must be one safe path component"};
  }
}

Scope parse_scope(std::string_view value) {
  if (value == "user") {
    return Scope::user;
  }
  if (value == "project") {
    return Scope::project;
  }
  throw Error{"--scope expects user or project"};
}

Source parse_source(std::string_view value) {
  if (value == "local") {
    return Source::local;
  }
  if (value == "published") {
    return Source::published;
  }
  throw Error{"--source expects local or published"};
}

} // namespace

std::vector<Client> known_clients(const Paths& paths) {
  return {
      {"claude", "claude", paths.home / ".claude.json", "mcpServers",
       ConfigFormat::json, EntryDialect::claude},
      {"codex", "codex", paths.home / ".codex/config.toml", "mcp_servers",
       ConfigFormat::toml, EntryDialect::standard},
      {"cursor", "cursor-agent", paths.home / ".cursor/mcp.json", "mcpServers",
       ConfigFormat::json, EntryDialect::standard},
      {"gemini", "gemini", paths.home / ".gemini/settings.json", "mcpServers",
       ConfigFormat::json, EntryDialect::standard},
      {"grok", "grok", paths.home / ".grok/config.toml", "mcp_servers",
       ConfigFormat::toml, EntryDialect::standard},
      {"agy", "agy", paths.home / ".gemini/config/mcp_config.json", "mcpServers",
       ConfigFormat::json, EntryDialect::standard},
      {"opencode", "opencode", paths.config_home / "opencode/opencode.jsonc", "mcp",
       ConfigFormat::jsonc, EntryDialect::opencode},
      {"pi", "pi", paths.home / ".pi/agent/mcp.json", "mcpServers", ConfigFormat::jsonc,
       EntryDialect::standard},
  };
}

std::vector<Client> select_clients(std::span<const Client> clients,
                                   std::span<const std::string> selectors) {
  if (selectors.empty()) {
    return {clients.begin(), clients.end()};
  }

  std::set<std::string, std::less<>> wanted;
  for (const auto& selector : selectors) {
    std::size_t start = 0;
    while (start <= selector.size()) {
      const auto comma = selector.find(',', start);
      const auto end = comma == std::string::npos ? selector.size() : comma;
      auto name =
          canonical_name(trim(std::string_view{selector}.substr(start, end - start)));
      if (name.empty()) {
        throw Error{"--cli selects an empty client"};
      }
      const auto known = std::ranges::find(clients, name, &Client::name);
      if (known == clients.end()) {
        throw Error{'"' + name +
                    "\" is not a client this tool knows: " + known_names(clients)};
      }
      wanted.insert(std::move(name));
      if (comma == std::string::npos) {
        break;
      }
      start = comma + 1;
    }
  }

  std::vector<Client> selected;
  std::ranges::copy_if(
      clients, std::back_inserter(selected),
      [&](const Client& client) { return wanted.contains(client.name); });
  return selected;
}

Options parse_arguments(std::span<const std::string> arguments) {
  if (arguments.empty()) {
    throw Error{"a command is required"};
  }
  Options result;
  result.command = parse_command(arguments.front());
  if (result.command == Command::help) {
    if (arguments.size() != 1) {
      throw Error{"help takes no arguments"};
    }
    return result;
  }

  for (std::size_t index = 1; index < arguments.size(); ++index) {
    auto [flag, assigned] = split_flag(arguments[index]);
    if (!flag.starts_with("--") || !accepts(result.command, flag)) {
      throw Error{'"' + arguments[index] + "\" is not valid for this command"};
    }
    const auto value = require_value(arguments, index, flag, std::move(assigned));
    if (flag == "--help") {
      result.command = Command::help;
    } else if (flag == "--dry-run") {
      result.dry_run = true;
    } else if (flag == "--no-preflight") {
      result.no_preflight = true;
    } else if (flag == "--repo") {
      result.repo = value;
    } else if (flag == "--source") {
      result.source = parse_source(value);
    } else if (flag == "--build-dir") {
      result.build_dir = value;
    } else if (flag == "--prefix") {
      result.prefix = value;
    } else if (flag == "--socket") {
      result.socket = value;
    } else if (flag == "--server") {
      result.server = value;
    } else if (flag == "--entry") {
      require_path_component(value, flag);
      result.entry = value;
    } else if (flag == "--cli" || flag == "--client") {
      result.clients.push_back(value);
    } else if (flag == "--scope") {
      result.scope = parse_scope(value);
    } else if (flag == "--env") {
      const auto equals = value.find('=');
      if (equals == std::string::npos || equals == 0) {
        throw Error{"--env expects KEY=VALUE"};
      }
      if (value.substr(0, equals) == "LIBTMUX_SAFETY") {
        throw Error{"LIBTMUX_SAFETY is retired; use LIBTMUX_TOOLSETS"};
      }
      result.environment.emplace_back(value.substr(0, equals),
                                      value.substr(equals + 1));
    }
  }
  if (result.prefix.has_value()) {
    result.source = Source::published;
  }
  if (result.build_dir.has_value()) {
    result.source = Source::local;
  }
  return result;
}

std::string usage() {
  return R"(usage: mcp-swap COMMAND [OPTIONS]

Commands:
  detect      Show supported clients and configuration presence
  status      Show the selected server entry without changing it
  use-local   Point selected clients at a local or published C++ server
  revert      Restore authenticated first-backup configuration bytes
  doctor      Report recovery, naming, and authentication hazards

Selection:
  --cli NAME[,NAME...]     Select clients; repeatable (alias: --client)
  --scope user|project     Select Claude scope; default project

use-local:
  --source local|published Choose a build in this checkout or an install
  --build-dir DIR          Read a local server from DIR; implies local
  --prefix DIR             Search DIR/bin and DIR/tools/libtmux; implies published
  --socket PATH            Pass one private POSIX tmux socket path
  --server NAME            Config key; default libtmux
  --entry NAME             Binary name; default from the CMake target
  --env KEY=VALUE          Merge an environment value; repeatable
  --no-preflight           Skip MCP initialize and tools/list validation
  --dry-run                Authenticate and print the plan without writing

status and doctor:
  --repo DIR               Search upward for the checkout; default .
  --server NAME            Config key; default libtmux

Exit status: 0 success, 1 runtime or safety refusal, 2 invalid arguments.
)";
}

} // namespace libtmux::mcp_swap
