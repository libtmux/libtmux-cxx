#include "mcp_swap.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>

namespace libtmux::mcp_swap {
namespace {

using Json = nlohmann::ordered_json;

constexpr std::string_view opencode_schema = "https://opencode.ai/config.json";
constexpr std::string_view retired_safety = "LIBTMUX_SAFETY";
constexpr std::string_view toolsets = "LIBTMUX_TOOLSETS";

void validate_utf8(std::string_view bytes) {
  const auto continuation = [](unsigned char byte) {
    return byte >= 0x80 && byte <= 0xbf;
  };
  for (std::size_t offset = 0; offset < bytes.size();) {
    const auto first = static_cast<unsigned char>(bytes[offset]);
    if (first <= 0x7f) {
      ++offset;
      continue;
    }

    std::size_t length{};
    unsigned char second_min = 0x80;
    unsigned char second_max = 0xbf;
    if (first >= 0xc2 && first <= 0xdf) {
      length = 2;
    } else if (first >= 0xe0 && first <= 0xef) {
      length = 3;
      if (first == 0xe0) {
        second_min = 0xa0;
      } else if (first == 0xed) {
        second_max = 0x9f;
      }
    } else if (first >= 0xf0 && first <= 0xf4) {
      length = 4;
      if (first == 0xf0) {
        second_min = 0x90;
      } else if (first == 0xf4) {
        second_max = 0x8f;
      }
    } else {
      throw Error{"configuration is not valid UTF-8"};
    }

    if (offset + length > bytes.size()) {
      throw Error{"configuration is not valid UTF-8"};
    }
    const auto second = static_cast<unsigned char>(bytes[offset + 1]);
    if (second < second_min || second > second_max) {
      throw Error{"configuration is not valid UTF-8"};
    }
    for (std::size_t index = 2; index < length; ++index) {
      if (!continuation(static_cast<unsigned char>(bytes[offset + index]))) {
        throw Error{"configuration is not valid UTF-8"};
      }
    }
    offset += length;
  }
}

std::string trim_copy(std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.remove_suffix(1);
  }
  return std::string{value};
}

std::filesystem::path canonical_path(const std::filesystem::path& path) {
  std::error_code error;
  auto result = std::filesystem::weakly_canonical(path, error);
  if (!error) {
    return result;
  }
  return path.is_absolute() ? path.lexically_normal()
                            : std::filesystem::absolute(path).lexically_normal();
}

std::filesystem::path claude_project_key(const std::filesystem::path& repository) {
  const auto fallback = canonical_path(repository);
  const auto dot_git = fallback / ".git";
  std::error_code error;
  if (std::filesystem::is_directory(dot_git, error)) {
    return fallback;
  }
  error.clear();
  if (!std::filesystem::is_regular_file(dot_git, error)) {
    return fallback;
  }

  std::ifstream pointer{dot_git};
  std::string line;
  if (!std::getline(pointer, line) || !line.starts_with("gitdir:")) {
    return fallback;
  }
  auto git_directory = std::filesystem::path{trim_copy(line.substr(7))};
  if (git_directory.empty()) {
    return fallback;
  }
  if (git_directory.is_relative()) {
    git_directory = dot_git.parent_path() / git_directory;
  }
  git_directory = canonical_path(git_directory);

  auto common_directory = git_directory;
  const auto common_file = git_directory / "commondir";
  error.clear();
  if (std::filesystem::is_regular_file(common_file, error)) {
    std::ifstream common_stream{common_file};
    std::string common;
    if (std::getline(common_stream, common) && !trim_copy(common).empty()) {
      common_directory = std::filesystem::path{trim_copy(common)};
      if (common_directory.is_relative()) {
        common_directory = git_directory / common_directory;
      }
      common_directory = canonical_path(common_directory);
    }
  } else if (git_directory.parent_path().filename() == "worktrees") {
    common_directory = git_directory.parent_path().parent_path();
  }
  if (common_directory.filename() != ".git") {
    return fallback;
  }
  return canonical_path(common_directory.parent_path());
}

std::vector<std::string> server_map_path(const Client& client,
                                         const std::filesystem::path& repository,
                                         Scope scope) {
  if (client.name == "claude" && scope == Scope::project) {
    return {"projects", claude_project_key(repository).string(), "mcpServers"};
  }
  return {client.container};
}

std::vector<std::string> entry_path(const Client& client,
                                    const std::filesystem::path& repository,
                                    Scope scope, std::string_view server) {
  auto path = server_map_path(client, repository, scope);
  path.emplace_back(server);
  return path;
}

Json parse_json(std::string_view bytes, bool comments) {
  if (trim_copy(bytes).empty()) {
    return Json::object();
  }
  std::string input{bytes};
  if (comments) {
    bool string = false;
    bool escaped = false;
    bool line = false;
    bool block = false;
    for (std::size_t index = 0; index < input.size(); ++index) {
      const char character = input[index];
      if (line) {
        if (character == '\n') {
          line = false;
        } else {
          input[index] = ' ';
        }
      } else if (block) {
        if (character == '*' && index + 1 < input.size() && input[index + 1] == '/') {
          input[index] = ' ';
          input[++index] = ' ';
          block = false;
        } else if (character != '\n') {
          input[index] = ' ';
        }
      } else if (string) {
        if (escaped) {
          escaped = false;
        } else if (character == '\\') {
          escaped = true;
        } else if (character == '"') {
          string = false;
        }
      } else if (character == '"') {
        string = true;
      } else if (character == '/' && index + 1 < input.size() &&
                 input[index + 1] == '/') {
        input[index] = ' ';
        input[++index] = ' ';
        line = true;
      } else if (character == '/' && index + 1 < input.size() &&
                 input[index + 1] == '*') {
        input[index] = ' ';
        input[++index] = ' ';
        block = true;
      }
    }
    if (string || block) {
      throw Error{"unterminated JSONC string or comment"};
    }
    string = false;
    escaped = false;
    std::optional<std::size_t> comma;
    for (std::size_t index = 0; index < input.size(); ++index) {
      const char character = input[index];
      if (string) {
        if (escaped) {
          escaped = false;
        } else if (character == '\\') {
          escaped = true;
        } else if (character == '"') {
          string = false;
        }
        continue;
      }
      if (character == '"') {
        string = true;
        comma.reset();
      } else if (character == ',') {
        comma = index;
      } else if (character == '}' || character == ']') {
        if (comma.has_value()) {
          input[*comma] = ' ';
        }
        comma.reset();
      } else if (std::isspace(static_cast<unsigned char>(character)) == 0) {
        comma.reset();
      }
    }
  }
  try {
    std::vector<std::set<std::string, std::less<>>> object_keys;
    const auto reject_duplicates = [&](int, Json::parse_event_t event, Json& parsed) {
      if (event == Json::parse_event_t::object_start) {
        object_keys.emplace_back();
      } else if (event == Json::parse_event_t::key) {
        if (object_keys.empty() ||
            !object_keys.back().insert(parsed.get<std::string>()).second) {
          throw Error{"configuration contains a duplicate JSON object key"};
        }
      } else if (event == Json::parse_event_t::object_end) {
        object_keys.pop_back();
      }
      return true;
    };
    auto result = Json::parse(input, reject_duplicates);
    if (!result.is_object()) {
      throw Error{"configuration root is not an object"};
    }
    return result;
  } catch (const nlohmann::json::exception& error) {
    throw Error{"configuration is not valid JSON: " + std::string{error.what()}};
  }
}

const Json* find_json(const Json& document, std::span<const std::string> path) {
  const Json* current = &document;
  for (const auto& key : path) {
    if (!current->is_object()) {
      throw Error{key + " is not an object"};
    }
    const auto found = current->find(key);
    if (found == current->end()) {
      return nullptr;
    }
    current = &*found;
  }
  return current;
}

Json& create_json(Json& document, std::span<const std::string> path) {
  Json* current = &document;
  for (const auto& key : path) {
    if (!current->is_object()) {
      throw Error{key + " is not an object"};
    }
    auto [found, inserted] = current->emplace(key, Json::object());
    static_cast<void>(inserted);
    if (!found->is_object()) {
      throw Error{key + " is not an object"};
    }
    current = &*found;
  }
  return *current;
}

std::vector<std::pair<std::string, std::string>> environment_of(const Json& entry,
                                                                EntryDialect dialect) {
  const auto key = dialect == EntryDialect::opencode ? "environment" : "env";
  const auto found = entry.find(key);
  if (found == entry.end() || found->is_null()) {
    return {};
  }
  if (!found->is_object()) {
    throw Error{std::string{key} + " is not an object"};
  }
  std::vector<std::pair<std::string, std::string>> result;
  for (const auto& [name, value] : found->items()) {
    if (!value.is_string()) {
      throw Error{std::string{key} + "." + name + " is not a string"};
    }
    result.emplace_back(name, value.get<std::string>());
  }
  return result;
}

ServerSpec spec_from_json(const Json& entry, EntryDialect dialect) {
  if (!entry.is_object()) {
    throw Error{"server entry is not an object"};
  }
  ServerSpec result;
  if (dialect == EntryDialect::opencode) {
    const auto command = entry.find("command");
    if (command == entry.end() || !command->is_array() || command->empty()) {
      throw Error{"opencode command is not a nonempty array"};
    }
    for (const auto& item : *command) {
      if (!item.is_string()) {
        throw Error{"opencode command contains a non-string argument"};
      }
    }
    result.command = command->front().get<std::string>();
    for (auto item = std::next(command->begin()); item != command->end(); ++item) {
      result.arguments.push_back(item->get<std::string>());
    }
  } else {
    const auto command = entry.find("command");
    if (command == entry.end() || !command->is_string()) {
      throw Error{"server command is not a string"};
    }
    result.command = command->get<std::string>();
    const auto arguments = entry.find("args");
    if (arguments != entry.end()) {
      if (!arguments->is_array()) {
        throw Error{"server args is not an array"};
      }
      for (const auto& argument : *arguments) {
        if (!argument.is_string()) {
          throw Error{"server args contains a non-string value"};
        }
        result.arguments.push_back(argument.get<std::string>());
      }
    }
  }
  result.environment = environment_of(entry, dialect);
  return result;
}

Json entry_from_spec(const ServerSpec& spec, EntryDialect dialect) {
  Json result = Json::object();
  if (dialect == EntryDialect::opencode) {
    result["type"] = "local";
    result["command"] = Json::array({spec.command});
    for (const auto& argument : spec.arguments) {
      result["command"].push_back(argument);
    }
    if (!spec.environment.empty()) {
      result["environment"] = Json::object();
      for (const auto& [name, value] : spec.environment) {
        result["environment"][name] = value;
      }
    }
    return result;
  }
  if (dialect == EntryDialect::claude) {
    result["type"] = "stdio";
  }
  result["command"] = spec.command;
  result["args"] = spec.arguments;
  if (dialect == EntryDialect::claude || !spec.environment.empty()) {
    result["env"] = Json::object();
    for (const auto& [name, value] : spec.environment) {
      result["env"][name] = value;
    }
  }
  return result;
}

struct EnvironmentMerge {
  std::vector<std::pair<std::string, std::string>> values;
  bool removed_safety{};
};

EnvironmentMerge
merge_environment(std::span<const std::pair<std::string, std::string>> existing,
                  std::span<const std::pair<std::string, std::string>> overrides) {
  std::map<std::string, std::string, std::less<>> environment;
  for (const auto& [name, value] : existing) {
    environment[name] = value;
  }
  const auto retired = environment.find(retired_safety);
  const bool replaces_safety = std::ranges::any_of(
      overrides, [](const auto& pair) { return pair.first == toolsets; });
  const bool removed_safety = retired != environment.end() && replaces_safety;
  if (removed_safety) {
    environment.erase(retired);
  }
  for (const auto& [name, value] : overrides) {
    if (name == retired_safety) {
      throw Error{"LIBTMUX_SAFETY is retired; use LIBTMUX_TOOLSETS"};
    }
    environment[name] = value;
  }
  return {{environment.begin(), environment.end()}, removed_safety};
}

Json merge_entry(const Json* existing, const ServerSpec& effective,
                 EntryDialect dialect) {
  Json merged = Json::object();
  if (existing != nullptr) {
    if (!existing->is_object()) {
      throw Error{"server entry is not an object"};
    }
    merged = *existing;
  }
  const auto fresh = entry_from_spec(effective, dialect);
  merged.erase("command");
  merged.erase("args");
  merged.erase("type");
  merged.erase("env");
  merged.erase("environment");
  merged.update(fresh);
  return merged;
}

std::size_t skip_space(std::string_view text, std::size_t offset) {
  while (offset < text.size() &&
         std::isspace(static_cast<unsigned char>(text[offset])) != 0) {
    ++offset;
  }
  return offset;
}

std::size_t end_string(std::string_view text, std::size_t offset) {
  ++offset;
  while (offset < text.size()) {
    if (text[offset] == '\\') {
      offset += std::min<std::size_t>(2, text.size() - offset);
    } else if (text[offset] == '"') {
      return offset + 1;
    } else {
      ++offset;
    }
  }
  throw Error{"unterminated JSONC string"};
}

std::size_t end_value(std::string_view text, std::size_t offset) {
  if (offset >= text.size()) {
    throw Error{"missing JSONC value"};
  }
  if (text[offset] == '"') {
    return end_string(text, offset);
  }
  if (text[offset] == '{' || text[offset] == '[') {
    std::vector<char> closing;
    while (offset < text.size()) {
      if (text[offset] == '"') {
        offset = end_string(text, offset);
        continue;
      }
      if (text[offset] == '{') {
        closing.push_back('}');
      } else if (text[offset] == '[') {
        closing.push_back(']');
      } else if (!closing.empty() && text[offset] == closing.back()) {
        closing.pop_back();
        if (closing.empty()) {
          return offset + 1;
        }
      }
      ++offset;
    }
    throw Error{"unterminated JSONC container"};
  }
  while (offset < text.size() && text[offset] != ',' && text[offset] != '}' &&
         text[offset] != ']' &&
         std::isspace(static_cast<unsigned char>(text[offset])) == 0) {
    ++offset;
  }
  return offset;
}

std::string blank_jsonc(std::string_view text) {
  std::string result{text};
  bool string = false;
  bool escaped = false;
  bool line = false;
  bool block = false;
  for (std::size_t index = 0; index < result.size(); ++index) {
    const char character = result[index];
    if (line) {
      if (character == '\n') {
        line = false;
      } else {
        result[index] = ' ';
      }
    } else if (block) {
      if (character == '*' && index + 1 < result.size() && result[index + 1] == '/') {
        result[index] = ' ';
        result[++index] = ' ';
        block = false;
      } else if (character != '\n') {
        result[index] = ' ';
      }
    } else if (string) {
      if (escaped) {
        escaped = false;
      } else if (character == '\\') {
        escaped = true;
      } else if (character == '"') {
        string = false;
      }
    } else if (character == '"') {
      string = true;
    } else if (character == '/' && index + 1 < result.size() &&
               result[index + 1] == '/') {
      result[index] = ' ';
      result[++index] = ' ';
      line = true;
    } else if (character == '/' && index + 1 < result.size() &&
               result[index + 1] == '*') {
      result[index] = ' ';
      result[++index] = ' ';
      block = true;
    }
  }
  return result;
}

struct JsoncSpan {
  std::size_t member_start{};
  std::size_t value_start{};
  std::size_t value_end{};
  std::optional<std::size_t> comma;
  std::size_t object_start{};
  std::size_t insertion{};
  bool present{};
  bool empty{};
  bool trailing_comma{};
};

JsoncSpan member_in(std::string_view text, std::size_t object, std::string_view key) {
  JsoncSpan result;
  result.object_start = object + 1;
  auto cursor = skip_space(text, result.object_start);
  result.empty = cursor < text.size() && text[cursor] == '}';
  result.insertion = result.object_start;
  while (cursor < text.size() && text[cursor] != '}') {
    cursor = skip_space(text, cursor);
    if (cursor < text.size() && text[cursor] == '}') {
      break;
    }
    if (cursor >= text.size() || text[cursor] != '"') {
      throw Error{"JSONC object contains an invalid member"};
    }
    const auto member_start = cursor;
    const auto name_end = end_string(text, cursor);
    std::string name;
    try {
      name = Json::parse(std::string{text.substr(cursor, name_end - cursor)})
                 .get<std::string>();
    } catch (const nlohmann::json::exception&) {
      throw Error{"JSONC object contains an invalid key"};
    }
    cursor = skip_space(text, name_end);
    if (cursor >= text.size() || text[cursor] != ':') {
      throw Error{"JSONC object member has no colon"};
    }
    const auto value_start = skip_space(text, cursor + 1);
    const auto value_end = end_value(text, value_start);
    auto after = skip_space(text, value_end);
    const bool comma = after < text.size() && text[after] == ',';
    result.insertion = comma ? after + 1 : value_end;
    result.trailing_comma = comma;
    if (name == key) {
      result.member_start = member_start;
      result.value_start = value_start;
      result.value_end = value_end;
      if (comma) {
        result.comma = after;
      }
      result.present = true;
      return result;
    }
    cursor = comma ? after + 1 : after;
  }
  if (cursor >= text.size() || text[cursor] != '}') {
    throw Error{"unterminated JSONC object"};
  }
  return result;
}

JsoncSpan find_jsonc(std::string_view blanked, std::span<const std::string> path) {
  auto object = skip_space(blanked, 0);
  if (object >= blanked.size() || blanked[object] != '{') {
    throw Error{"JSONC root is not an object"};
  }
  for (std::size_t depth = 0; depth < path.size(); ++depth) {
    auto result = member_in(blanked, object, path[depth]);
    if (depth + 1 == path.size()) {
      return result;
    }
    if (!result.present) {
      return result;
    }
    object = skip_space(blanked, result.value_start);
    if (object >= blanked.size() || blanked[object] != '{') {
      throw Error{path[depth] + " is not a JSONC object"};
    }
  }
  throw Error{"empty JSONC path"};
}

std::string indent_json(std::string rendered, std::size_t spaces) {
  const std::string prefix(spaces, ' ');
  std::size_t newline = 0;
  while ((newline = rendered.find('\n', newline)) != std::string::npos) {
    rendered.insert(++newline, prefix);
    newline += spaces;
  }
  return rendered;
}

std::string set_jsonc(std::string text, std::span<const std::string> path,
                      const Json& value) {
  if (trim_copy(text).empty()) {
    text = "{}\n";
  }
  const auto blanked = blank_jsonc(text);
  auto span = find_jsonc(blanked, path);
  if (!span.present && path.size() > 1) {
    const auto parent_path = path.first(path.size() - 1);
    auto parent = find_jsonc(blanked, parent_path);
    if (!parent.present) {
      Json nested = Json::object();
      nested[path.back()] = value;
      return set_jsonc(std::move(text), parent_path, nested);
    }
  }
  const auto rendered = indent_json(value.dump(2), path.size() * 2);
  if (span.present) {
    text.replace(span.value_start, span.value_end - span.value_start, rendered);
    return text;
  }
  const std::string member = (span.empty || span.trailing_comma ? "" : ",") +
                             std::string{"\n"} + std::string(path.size() * 2, ' ') +
                             Json(path.back()).dump() + ": " + rendered;
  text.insert(span.insertion, member);
  return text;
}

std::string remove_jsonc(std::string text, std::span<const std::string> path) {
  const auto span = find_jsonc(blank_jsonc(text), path);
  if (!span.present) {
    return text;
  }
  for (std::size_t index = span.member_start; index < span.value_end; ++index) {
    if (text[index] != '\n' && text[index] != '\r') {
      text[index] = ' ';
    }
  }
  if (span.comma.has_value()) {
    text[*span.comma] = ' ';
  }
  return text;
}

std::string set_jsonc_server(std::string text, std::span<const std::string> path,
                             const Json* prior, const Json& merged,
                             const ServerSpec& replacement, EntryDialect dialect,
                             bool remove_safety) {
  if (prior == nullptr) {
    return set_jsonc(std::move(text), path, merged);
  }
  const auto set_member = [&](std::string current, std::string_view name,
                              const Json& value) {
    std::vector<std::string> member{path.begin(), path.end()};
    member.emplace_back(name);
    return set_jsonc(std::move(current), member, value);
  };

  if (merged.contains("type")) {
    text = set_member(std::move(text), "type", merged["type"]);
  }
  text = set_member(std::move(text), "command", merged["command"]);
  if (merged.contains("args")) {
    text = set_member(std::move(text), "args", merged["args"]);
  }
  const auto environment_name =
      dialect == EntryDialect::opencode ? "environment" : "env";
  if (remove_safety) {
    std::vector<std::string> member{path.begin(), path.end()};
    member.emplace_back(environment_name);
    member.emplace_back(retired_safety);
    text = remove_jsonc(std::move(text), member);
  }
  if (!merged.contains(environment_name)) {
    return text;
  }
  const auto existing = prior->find(environment_name);
  if (existing == prior->end() || !existing->is_object()) {
    return set_member(std::move(text), environment_name, merged[environment_name]);
  }
  for (const auto& [name, value] : replacement.environment) {
    std::vector<std::string> member{path.begin(), path.end()};
    member.emplace_back(environment_name);
    member.push_back(name);
    text = set_jsonc(std::move(text), member, merged[environment_name][name]);
  }
  return text;
}

std::string json_bytes(const Json& document, std::string_view original) {
  const bool newline = original.empty() || original.ends_with('\n');
  auto rendered = document.dump(2);
  if (newline) {
    rendered.push_back('\n');
  }
  return rendered;
}

std::string toml_quote(std::string_view value) {
  return Json(std::string{value}).dump();
}

std::string toml_segment(std::string_view value) {
  const bool bare = !value.empty() && std::ranges::all_of(value, [](char character) {
    const auto byte = static_cast<unsigned char>(character);
    return std::isalnum(byte) != 0 || character == '_' || character == '-';
  });
  return bare ? std::string{value} : toml_quote(value);
}

struct TableSpan {
  std::size_t header{};
  std::size_t body{};
  std::size_t end{};
  bool found{};
};

std::string_view line_without_comment(std::string_view line) {
  bool basic = false;
  bool literal = false;
  bool escaped = false;
  for (std::size_t index = 0; index < line.size(); ++index) {
    const char character = line[index];
    if (basic) {
      if (escaped) {
        escaped = false;
      } else if (character == '\\') {
        escaped = true;
      } else if (character == '"') {
        basic = false;
      }
    } else if (literal) {
      if (character == '\'') {
        literal = false;
      }
    } else if (character == '"') {
      basic = true;
    } else if (character == '\'') {
      literal = true;
    } else if (character == '#') {
      return line.substr(0, index);
    }
  }
  return line;
}

TableSpan find_table(std::string_view text, std::string_view name) {
  const auto expected = '[' + std::string{name} + ']';
  TableSpan result;
  std::size_t offset = 0;
  while (offset < text.size()) {
    const auto newline = text.find('\n', offset);
    const auto line_end = newline == std::string_view::npos ? text.size() : newline + 1;
    const auto line = text.substr(offset, line_end - offset);
    const auto clean = trim_copy(line_without_comment(line));
    if (!result.found && clean == expected) {
      result = {offset, line_end, text.size(), true};
    } else if (result.found && !clean.empty() && clean.front() == '[') {
      result.end = offset;
      return result;
    }
    offset = line_end;
  }
  return result;
}

struct AssignmentSpan {
  std::size_t assignment_start{};
  std::size_t value_start{};
  std::size_t value_end{};
  bool found{};
};

std::optional<std::string> parse_toml_string(std::string_view raw);

std::optional<std::size_t> assignment_equals(std::string_view line) {
  bool basic = false;
  bool literal = false;
  bool escaped = false;
  for (std::size_t index = 0; index < line.size(); ++index) {
    const char character = line[index];
    if (basic) {
      if (escaped) {
        escaped = false;
      } else if (character == '\\') {
        escaped = true;
      } else if (character == '"') {
        basic = false;
      }
    } else if (literal) {
      if (character == '\'') {
        literal = false;
      }
    } else if (character == '"') {
      basic = true;
    } else if (character == '\'') {
      literal = true;
    } else if (character == '#') {
      return std::nullopt;
    } else if (character == '=') {
      return index;
    }
  }
  return std::nullopt;
}

std::size_t toml_value_end(std::string_view text, std::size_t start,
                           std::size_t limit) {
  enum class Quote { none, basic, literal, multiline_basic, multiline_literal };
  Quote quote = Quote::none;
  int square = 0;
  int curly = 0;
  bool saw_value = false;
  std::size_t last = start;
  for (std::size_t index = start; index < limit; ++index) {
    const char character = text[index];
    if (quote == Quote::basic) {
      if (character == '\\') {
        if (++index >= limit) {
          break;
        }
      } else if (character == '"') {
        quote = Quote::none;
      }
      last = index + 1;
      continue;
    }
    if (quote == Quote::literal) {
      if (character == '\'') {
        quote = Quote::none;
      }
      last = index + 1;
      continue;
    }
    if (quote == Quote::multiline_basic) {
      if (character == '\\') {
        if (++index >= limit) {
          break;
        }
      } else if (text.substr(index, 3) == "\"\"\"") {
        quote = Quote::none;
        index += 2;
      }
      last = index + 1;
      continue;
    }
    if (quote == Quote::multiline_literal) {
      if (text.substr(index, 3) == "'''") {
        quote = Quote::none;
        index += 2;
      }
      last = index + 1;
      continue;
    }

    if (character == '#' && square == 0 && curly == 0) {
      return last;
    }
    if (character == '#') {
      const auto newline = text.find('\n', index);
      if (newline == std::string_view::npos || newline >= limit) {
        break;
      }
      index = newline;
      continue;
    }
    if (character == '\n' || character == '\r') {
      if (square == 0 && curly == 0 && saw_value) {
        return last;
      }
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(character)) != 0) {
      continue;
    }
    saw_value = true;
    if (text.substr(index, 3) == "\"\"\"") {
      quote = Quote::multiline_basic;
      index += 2;
    } else if (text.substr(index, 3) == "'''") {
      quote = Quote::multiline_literal;
      index += 2;
    } else if (character == '"') {
      quote = Quote::basic;
    } else if (character == '\'') {
      quote = Quote::literal;
    } else if (character == '[') {
      ++square;
    } else if (character == ']') {
      --square;
    } else if (character == '{') {
      ++curly;
    } else if (character == '}') {
      --curly;
    }
    if (square < 0 || curly < 0) {
      throw Error{"TOML value has an unmatched closing delimiter"};
    }
    last = index + 1;
  }
  if (saw_value && quote == Quote::none && square == 0 && curly == 0) {
    return last;
  }
  throw Error{"TOML value is incomplete"};
}

std::optional<std::string> parse_toml_key(std::string_view raw) {
  const auto key = trim_copy(raw);
  if (key.empty()) {
    return std::nullopt;
  }
  if (key.front() == '"' || key.front() == '\'') {
    return parse_toml_string(key);
  }
  if (!std::ranges::all_of(key, [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return std::isalnum(byte) != 0 || character == '_' || character == '-';
      })) {
    return std::nullopt;
  }
  return key;
}

toml::table parse_toml_document(std::string_view text) {
  auto parsed = toml::parse(text);
  if (!parsed) {
    throw Error{"configuration is not valid TOML: " +
                std::string{parsed.error().description()}};
  }
  return std::move(parsed).table();
}

AssignmentSpan find_assignment(std::string_view text, const TableSpan& table,
                               std::string_view key) {
  if (!table.found) {
    return {};
  }
  std::size_t offset = table.body;
  while (offset < table.end) {
    const auto newline = text.find('\n', offset);
    const auto line_end = newline == std::string_view::npos
                              ? table.end
                              : std::min(table.end, newline + 1);
    const auto line = text.substr(offset, line_end - offset);
    const auto equals = assignment_equals(line);
    if (equals.has_value()) {
      auto start = offset + *equals + 1;
      while (start < line_end && (text[start] == ' ' || text[start] == '\t')) {
        ++start;
      }
      const auto end = toml_value_end(text, start, table.end);
      const auto parsed_key = parse_toml_key(line.substr(0, *equals));
      if (parsed_key.has_value() && *parsed_key == key) {
        return {offset, start, end, true};
      }
      const auto after = text.find('\n', end);
      offset = after == std::string_view::npos ? table.end : after + 1;
      continue;
    }
    offset = line_end;
  }
  return {};
}

std::optional<std::string> parse_toml_string(std::string_view raw) {
  const auto value = trim_copy(raw);
  if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
    return value.substr(1, value.size() - 2);
  }
  try {
    const auto parsed = Json::parse(value);
    if (parsed.is_string()) {
      return parsed.get<std::string>();
    }
  } catch (const nlohmann::json::exception&) {
  }
  return std::nullopt;
}

std::string render_toml_inline_environment(
    std::span<const std::pair<std::string, std::string>> environment) {
  std::string result{"{"};
  for (std::size_t index = 0; index < environment.size(); ++index) {
    if (index != 0) {
      result += ',';
    }
    result += ' ' + toml_segment(environment[index].first) + " = " +
              toml_quote(environment[index].second);
  }
  if (!environment.empty()) {
    result.push_back(' ');
  }
  result.push_back('}');
  return result;
}

ServerSpec spec_from_toml(const toml::table& entry) {
  ServerSpec result;
  const auto* command = entry.get("command");
  if (command == nullptr || !command->is_string()) {
    throw Error{"TOML server command is not a string"};
  }
  result.command = *command->value<std::string>();

  if (const auto* arguments = entry.get("args"); arguments != nullptr) {
    const auto* array = arguments->as_array();
    if (array == nullptr) {
      throw Error{"TOML server args is not an array"};
    }
    for (const auto& argument : *array) {
      const auto value = argument.value<std::string>();
      if (!value.has_value()) {
        throw Error{"TOML server args contains a non-string value"};
      }
      result.arguments.push_back(*value);
    }
  }

  if (const auto* environment = entry.get("env"); environment != nullptr) {
    const auto* table = environment->as_table();
    if (table == nullptr) {
      throw Error{"TOML server env is not a table"};
    }
    for (const auto& [name, node] : *table) {
      const auto value = node.value<std::string>();
      if (!value.has_value()) {
        throw Error{"TOML server env contains a non-string value"};
      }
      result.environment.emplace_back(std::string{name.str()}, *value);
    }
    std::ranges::sort(result.environment, {},
                      &std::pair<std::string, std::string>::first);
  }
  return result;
}

const toml::table* toml_server_map(const toml::table& document, const Client& client) {
  const auto* container_node = document.get(client.container);
  if (container_node == nullptr) {
    return nullptr;
  }
  const auto* container = container_node->as_table();
  if (container == nullptr) {
    throw Error{client.container + " is not a TOML table"};
  }
  return container;
}

std::optional<ServerSpec> read_toml(const Client& client, std::string_view bytes,
                                    std::string_view server) {
  const auto document = parse_toml_document(bytes);
  const auto* container = toml_server_map(document, client);
  if (container == nullptr) {
    return std::nullopt;
  }
  const auto* entry_node = container->get(server);
  if (entry_node == nullptr) {
    return std::nullopt;
  }
  const auto* entry = entry_node->as_table();
  if (entry == nullptr) {
    throw Error{"TOML server entry is not a table"};
  }
  return spec_from_toml(*entry);
}

std::vector<std::pair<std::string, ServerSpec>>
read_toml_servers(const Client& client, std::string_view bytes) {
  const auto document = parse_toml_document(bytes);
  const auto* container = toml_server_map(document, client);
  std::vector<std::pair<std::string, ServerSpec>> result;
  if (container == nullptr) {
    return result;
  }
  for (const auto& [name, node] : *container) {
    const auto* entry = node.as_table();
    if (entry == nullptr) {
      continue;
    }
    result.emplace_back(std::string{name.str()}, spec_from_toml(*entry));
  }
  std::ranges::sort(result, {}, &std::pair<std::string, ServerSpec>::first);
  return result;
}

void replace_or_append(std::string& text, std::string_view table_name,
                       std::string_view key, std::string value) {
  auto table = find_table(text, table_name);
  if (!table.found) {
    throw Error{"TOML table disappeared while it was edited"};
  }
  const auto assignment = find_assignment(text, table, key);
  if (assignment.found) {
    text.replace(assignment.value_start, assignment.value_end - assignment.value_start,
                 value);
    return;
  }
  std::string insertion = std::string{key} + " = " + value + "\n";
  text.insert(table.end, insertion);
}

void remove_assignment(std::string& text, std::string_view table_name,
                       std::string_view key) {
  const auto assignment = find_assignment(text, find_table(text, table_name), key);
  if (!assignment.found) {
    return;
  }
  for (std::size_t index = assignment.assignment_start; index < assignment.value_end;
       ++index) {
    if (text[index] != '\n' && text[index] != '\r') {
      text[index] = ' ';
    }
  }
}

std::string render_toml(const Client& client, std::string_view bytes,
                        std::string_view server, const ServerSpec& effective,
                        std::span<const std::pair<std::string, std::string>> overrides,
                        bool remove_safety) {
  std::string result{bytes};
  const auto base = client.container + "." + toml_segment(server);
  if (!find_table(result, base).found) {
    if (!result.empty() && !result.ends_with('\n')) {
      result.push_back('\n');
    }
    if (!result.empty() && !result.ends_with("\n\n")) {
      result.push_back('\n');
    }
    result += '[' + base + "]\ncommand = " + toml_quote(effective.command) +
              "\nargs = " + Json(effective.arguments).dump() + "\n";
  } else {
    replace_or_append(result, base, "command", toml_quote(effective.command));
    replace_or_append(result, base, "args", Json(effective.arguments).dump());
  }
  if (!overrides.empty() || remove_safety) {
    const auto base_table = find_table(result, base);
    const auto inline_environment = find_assignment(result, base_table, "env");
    if (inline_environment.found) {
      result.replace(inline_environment.value_start,
                     inline_environment.value_end - inline_environment.value_start,
                     render_toml_inline_environment(effective.environment));
      return result;
    }
    const auto environment_table = base + ".env";
    if (!find_table(result, environment_table).found) {
      const auto base_table = find_table(result, base);
      result.insert(base_table.end, "\n[" + environment_table + "]\n");
    }
    if (remove_safety) {
      remove_assignment(result, environment_table, retired_safety);
    }
    for (const auto& [name, value] : overrides) {
      replace_or_append(result, environment_table, toml_segment(name),
                        toml_quote(value));
    }
  }
  return result;
}

std::optional<ServerSpec> read_json_server(const Client& client, std::string_view bytes,
                                           std::string_view server,
                                           const std::filesystem::path& repository,
                                           Scope scope) {
  const auto document = parse_json(bytes, client.format == ConfigFormat::jsonc);
  const auto path = entry_path(client, repository, scope, server);
  const auto entry = find_json(document, path);
  if (entry == nullptr) {
    return std::nullopt;
  }
  return spec_from_json(*entry, client.dialect);
}

std::vector<std::pair<std::string, ServerSpec>>
read_json_servers(const Client& client, std::string_view bytes,
                  const std::filesystem::path& repository, Scope scope) {
  const auto document = parse_json(bytes, client.format == ConfigFormat::jsonc);
  const auto path = server_map_path(client, repository, scope);
  const auto* servers = find_json(document, path);
  std::vector<std::pair<std::string, ServerSpec>> result;
  if (servers == nullptr) {
    return result;
  }
  if (!servers->is_object()) {
    throw Error{client.container + " is not an object"};
  }
  for (const auto& [name, entry] : servers->items()) {
    if (!entry.is_object()) {
      continue;
    }
    result.emplace_back(name, spec_from_json(entry, client.dialect));
  }
  return result;
}

} // namespace

std::optional<ServerSpec> read_server(const Client& client, std::string_view bytes,
                                      std::string_view server,
                                      const std::filesystem::path& repository,
                                      Scope scope) {
  validate_utf8(bytes);
  if (client.format == ConfigFormat::toml) {
    return read_toml(client, bytes, server);
  }
  return read_json_server(client, bytes, server, repository, scope);
}

std::vector<std::pair<std::string, ServerSpec>>
read_servers(const Client& client, std::string_view bytes,
             const std::filesystem::path& repository, Scope scope) {
  validate_utf8(bytes);
  if (client.format == ConfigFormat::toml) {
    return read_toml_servers(client, bytes);
  }
  return read_json_servers(client, bytes, repository, scope);
}

RenderedConfig render_server(const Client& client, std::string_view bytes,
                             std::string_view server, const ServerSpec& replacement,
                             const std::filesystem::path& repository, Scope scope) {
  validate_utf8(bytes);
  if (replacement.command.empty()) {
    throw Error{"server command is empty"};
  }
  const auto existing = read_server(client, bytes, server, repository, scope);
  std::span<const std::pair<std::string, std::string>> inherited_environment;
  if (existing.has_value()) {
    inherited_environment = existing->environment;
  }
  const auto environment =
      merge_environment(inherited_environment, replacement.environment);
  auto effective = replacement;
  effective.environment = environment.values;
  if (client.format == ConfigFormat::toml) {
    auto rendered = render_toml(client, bytes, server, effective,
                                replacement.environment, environment.removed_safety);
    const auto reparsed = read_toml(client, rendered, server);
    if (!reparsed.has_value() || *reparsed != effective) {
      throw Error{"rendered TOML server entry did not validate"};
    }
    return {std::move(rendered),
            existing.has_value() ? ChangeAction::replaced : ChangeAction::added,
            std::move(effective)};
  }

  auto document = parse_json(bytes, client.format == ConfigFormat::jsonc);
  const auto path = entry_path(client, repository, scope, server);
  const auto* found = find_json(document, path);
  const std::optional<Json> prior =
      found == nullptr ? std::nullopt : std::optional<Json>{*found};
  const auto merged =
      merge_entry(prior.has_value() ? &*prior : nullptr, effective, client.dialect);
  if (client.name == "claude" && scope == Scope::project) {
    const std::array project_path{std::string{"projects"},
                                  claude_project_key(repository).string()};
    auto& project = create_json(document, project_path);
    if (project.empty()) {
      project["allowedTools"] = Json::array();
      project["mcpContextUris"] = Json::array();
      project["mcpServers"] = Json::object();
      project["env"] = Json::object();
    }
  }
  auto& parent = create_json(document, std::span{path}.first(path.size() - 1));
  parent[path.back()] = merged;
  if (client.dialect == EntryDialect::opencode && document.size() == 1 &&
      document.contains(client.container)) {
    document["$schema"] = opencode_schema;
  }

  std::string rendered;
  if (client.format == ConfigFormat::json) {
    rendered = json_bytes(document, bytes);
  } else {
    rendered = std::string{bytes};
    if (document.contains("$schema")) {
      const auto original = parse_json(bytes, true);
      if (!original.contains("$schema")) {
        const std::array schema_path{std::string{"$schema"}};
        rendered = set_jsonc(std::move(rendered), schema_path, document["$schema"]);
      }
    }
    rendered = set_jsonc_server(
        std::move(rendered), path, prior.has_value() ? &*prior : nullptr, merged,
        replacement, client.dialect, environment.removed_safety);
    static_cast<void>(parse_json(rendered, true));
  }
  return {std::move(rendered),
          existing.has_value() ? ChangeAction::replaced : ChangeAction::added,
          std::move(effective)};
}

} // namespace libtmux::mcp_swap
