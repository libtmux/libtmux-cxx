#include "services.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <unistd.h>

#include "libtmux/server.hpp"
#include "libtmux_consumers/tmuxp.hpp"
#include <yaml-cpp/yaml.h>

namespace libtmux::workspace::cli {
namespace {
namespace fs = std::filesystem;
std::string environment(const char* name) {
  const auto* value = std::getenv(name);
  return value == nullptr ? std::string{} : std::string{value};
}
std::string expand(std::string value) {
  if (value == "~" || value.starts_with("~/"))
    value.replace(0, 1, environment("HOME"));
  for (std::size_t at = 0; at < value.size();) {
    if (value[at] != '$') {
      ++at;
      continue;
    }
    const bool braces = at + 1 < value.size() && value[at + 1] == '{';
    const auto begin = at + (braces ? 2U : 1U);
    auto end = begin;
    while (end < value.size() &&
           (std::isalnum(static_cast<unsigned char>(value[end])) != 0 ||
            value[end] == '_'))
      ++end;
    if (end == begin || (braces && (end == value.size() || value[end] != '}'))) {
      ++at;
      continue;
    }
    const auto name = value.substr(begin, end - begin);
    const auto* replacement = std::getenv(name.c_str());
    const auto length = end - at + (braces ? 1U : 0U);
    if (replacement == nullptr)
      at += length;
    else {
      value.replace(at, length, replacement);
      at += std::char_traits<char>::length(replacement);
    }
  }
  return value;
}
std::string private_path(const fs::path& path) {
  auto value = path.string();
  const auto home = environment("HOME");
  if (!home.empty() && (value == home || value.starts_with(home + "/")))
    value.replace(0, home.size(), "~");
  return value;
}
Json from_yaml(const YAML::Node& node, int depth = 0) {
  if (depth > 64)
    throw Failure{1, "INVALID_CONFIG", "workspace nesting exceeds 64 levels"};
  if (!node || node.IsNull())
    return nullptr;
  if (node.IsSequence()) {
    Json array = Json::array();
    for (const auto& child : node)
      array.push_back(from_yaml(child, depth + 1));
    return array;
  }
  if (node.IsMap()) {
    Json object = Json::object();
    for (const auto& entry : node) {
      if (!entry.first.IsScalar())
        throw Failure{1, "INVALID_CONFIG", "mapping keys must be strings"};
      const auto key = entry.first.Scalar();
      if (object.contains(key))
        throw Failure{1, "INVALID_CONFIG", "duplicate mapping key: " + key};
      object[key] = from_yaml(entry.second, depth + 1);
    }
    return object;
  }
  const auto text = node.Scalar();
  if (node.Tag() == "!" || node.Tag() == "tag:yaml.org,2002:str")
    return text;
  bool boolean{};
  if (YAML::convert<bool>::decode(node, boolean))
    return boolean;
  long long integer{};
  const auto number = std::from_chars(text.data(), text.data() + text.size(), integer);
  if (number.ec == std::errc{} && number.ptr == text.data() + text.size())
    return integer;
  char* end{};
  const double real = std::strtod(text.c_str(), &end);
  if (end != text.c_str() && end == text.c_str() + text.size() && std::isfinite(real))
    return real;
  return text;
}
Json read_document(const fs::path& path) {
  std::ifstream file{path, std::ios::binary};
  if (!file)
    throw Failure{1, "READ_FAILED", "cannot read " + private_path(path)};
  std::string contents;
  char buffer[8192];
  while (file.read(buffer, sizeof buffer) || file.gcount() != 0) {
    contents.append(buffer, static_cast<std::size_t>(file.gcount()));
    if (contents.size() > 4U * 1024U * 1024U)
      throw Failure{1, "INVALID_CONFIG", "workspace exceeds 4 MiB"};
  }
  Json result;
  if (path.extension() == ".json")
    result = Json::parse(contents);
  else {
    const auto documents = YAML::LoadAll(contents);
    if (documents.size() != 1)
      throw Failure{1, "INVALID_CONFIG", "exactly one YAML document is required"};
    result = from_yaml(documents.front());
  }
  if (!result.is_object())
    throw Failure{1, "INVALID_CONFIG", "workspace must be a mapping"};
  return result;
}
std::vector<fs::path> global_directories() {
  std::vector<fs::path> directories;
  const auto explicit_dir = environment("TMUXP_CONFIGDIR");
  if (!explicit_dir.empty())
    directories.emplace_back(expand(explicit_dir));
  const auto home = environment("HOME");
  if (!home.empty())
    directories.emplace_back(fs::path{home} / ".tmuxp");
  auto xdg = environment("XDG_CONFIG_HOME");
  if (xdg.empty() && !home.empty())
    xdg = (fs::path{home} / ".config").string();
  if (!xdg.empty())
    directories.emplace_back(fs::path{xdg} / "tmuxp");
  std::vector<fs::path> unique;
  for (const auto& dir : directories) {
    const auto absolute = fs::absolute(dir).lexically_normal();
    if (std::ranges::find(unique, absolute) == unique.end())
      unique.push_back(absolute);
  }
  return unique;
}
bool workspace_extension(const fs::path& path) {
  return path.extension() == ".yaml" || path.extension() == ".yml" ||
         path.extension() == ".json";
}
struct WorkspaceFile {
  fs::path path;
  std::string source;
};
std::vector<WorkspaceFile> discover() {
  std::vector<WorkspaceFile> result;
  std::set<fs::path> seen;
  const auto add = [&](const fs::path& path, const char* source) {
    std::error_code error;
    if (fs::is_regular_file(path, error) &&
        seen.insert(fs::absolute(path).lexically_normal()).second)
      result.push_back({fs::absolute(path).lexically_normal(), source});
  };
  auto directory = fs::current_path();
  for (;;) {
    for (const auto* extension : {".yaml", ".yml", ".json"})
      add(directory / (".tmuxp" + std::string{extension}), "local");
    const auto parent = directory.parent_path();
    if (parent == directory)
      break;
    directory = parent;
  }
  for (const auto& dir : global_directories()) {
    std::error_code error;
    fs::directory_iterator entries{dir, error};
    if (error)
      continue;
    std::vector<fs::path> sorted;
    for (const auto& entry : entries)
      if (workspace_extension(entry.path()))
        sorted.push_back(entry.path());
    std::ranges::sort(sorted);
    for (const auto& path : sorted)
      add(path, "global");
  }
  return result;
}
fs::path resolve(const std::string& name, const std::string& importer = {}) {
  const fs::path input{expand(name)};
  const bool explicit_path =
      input.is_absolute() || input.has_parent_path() || input.has_extension();
  std::vector<fs::path> candidates;
  if (explicit_path || importer.empty())
    candidates.push_back(input);
  if (fs::is_directory(input)) {
    for (const auto* extension : {".yaml", ".yml", ".json"})
      candidates.push_back(input / (".tmuxp" + std::string{extension}));
  }
  if (!explicit_path) {
    if (importer.empty()) {
      for (const auto& dir : global_directories())
        for (const auto* ext : {".yaml", ".yml", ".json"})
          candidates.push_back(dir / (name + ext));
    } else {
      const auto custom =
          importer == "tmuxinator" ? environment("TMUXINATOR_CONFIG") : std::string{};
      const fs::path base = custom.empty()
                                ? fs::path{environment("HOME")} / ("." + importer)
                                : fs::path{expand(custom)};
      candidates.push_back(base / (name + ".yml"));
      candidates.push_back(base / (name + ".yaml"));
    }
  }
  for (const auto& path : candidates)
    if (fs::is_regular_file(path))
      return fs::absolute(path).lexically_normal();
  throw Failure{1, "WORKSPACE_NOT_FOUND", "workspace not found: " + name};
}
Json record(const WorkspaceFile& file, bool full) {
  const auto modified = fs::last_write_time(file.path);
  const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      modified - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
  const std::time_t stamp = std::chrono::system_clock::to_time_t(system);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &stamp);
#else
  gmtime_r(&stamp, &utc);
#endif
  std::ostringstream time;
  time << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  Json config = nullptr;
  try {
    config = read_document(file.path);
  } catch (const std::exception&) {
  }
  Json result{{"name", file.path.stem().string()},
              {"path", private_path(file.path)},
              {"format", file.path.extension() == ".json" ? "json" : "yaml"},
              {"size", fs::file_size(file.path)},
              {"mtime", time.str()},
              {"session_name",
               config.is_object() ? config.value("session_name", Json{}) : Json{}},
              {"source", file.source}};
  if (full)
    result["config"] = config;
  return result;
}
void normalise(Json& node, const fs::path& base, const fs::path& parent = {}) {
  if (!node.is_object())
    return;
  for (const auto* key : {"session_name", "window_name"})
    if (node.contains(key) && node[key].is_string())
      node[key] = expand(node[key].get<std::string>());
  if (!node.contains("start_directory") && node.contains("root")) {
    node["start_directory"] = node["root"];
    node.erase("root");
  }
  fs::path directory = parent;
  if (node.contains("start_directory") && node["start_directory"].is_string()) {
    directory = expand(node["start_directory"].get<std::string>());
    if (directory.is_relative())
      directory = (parent.empty() ? base : parent) / directory;
    directory = directory.lexically_normal();
    node["start_directory"] = directory.string();
  }
  for (const auto* key :
       {"environment", "global_options", "options", "options_after"}) {
    if (!node.contains(key) || !node[key].is_object())
      continue;
    for (auto& value : node[key]) {
      if (value.is_boolean() && std::string_view{key} != "environment")
        value = value.get<bool>() ? "on" : "off";
      else if (value.is_string()) {
        auto text = expand(value.get<std::string>());
        if (text.starts_with("."))
          text = (base / text).lexically_normal().string();
        value = text;
      }
    }
  }
  for (const auto* key : {"windows", "panes"}) {
    if (!node.contains(key) || !node[key].is_array())
      continue;
    for (auto& child : node[key])
      normalise(child, base, directory);
  }
}
Server endpoint(const Request& request) {
  auto server =
      !request.value("S").empty()   ? Server::at_socket_path(expand(request.value("S")))
      : !request.value("L").empty() ? Server::at_socket_name(request.value("L"))
      : !environment("TMUX").empty() ? Server::from_env()
                                     : Server::at_default();
  if (!server)
    throw Failure{1, "INVALID_ENDPOINT", server.error().diagnostic};
  return *server;
}
libtmux::Pane current_pane(const Server& server, const std::string& code) {
  const auto inherited = Server::from_env();
  if (!inherited)
    throw Failure{1, code, inherited.error().diagnostic};
  const auto pane = inherited->pane(environment("TMUX_PANE"));
  if (!pane)
    throw Failure{1, code, pane.error().diagnostic};
  const auto context = environment("TMUX");
  const auto last = context.find_last_of(',');
  const auto previous = last == std::string::npos || last == 0
                            ? std::string::npos
                            : context.find_last_of(',', last - 1);
  const auto pid = pane->expand("#{pid}");
  if (previous == std::string::npos || !pid ||
      *pid != context.substr(previous + 1, last - previous - 1))
    throw Failure{1, code, "TMUX identifies a different server process"};
  const auto selected = server.pane(environment("TMUX_PANE"));
  if (!selected || selected->connection_identity() != pane->connection_identity())
    throw Failure{1, code, "selected server differs from the current pane's server"};
  return *selected;
}
Session append_target(const Server& server) {
  const auto session = current_pane(server, "APPEND_CONTEXT").session();
  if (!session)
    throw Failure{1, "APPEND_CONTEXT", session.error().diagnostic};
  return *session;
}
Client current_client(const Server& server, std::string_view pane) {
  const auto clients = server.clients();
  if (!clients)
    throw Failure{1, "CLIENT_CONTEXT", clients.error().diagnostic};
  std::optional<Client> selected;
  for (const auto& client : *clients) {
    if (client.control_mode() || client.tty().empty() ||
        client.active_pane_id() != pane)
      continue;
    if (selected)
      throw Failure{2, "CLIENT_CONTEXT",
                    "multiple clients view this pane; use load -d"};
    selected = client;
  }
  if (!selected || selected->pid() <= 0 || selected->name().empty())
    throw Failure{2, "CLIENT_CONTEXT",
                  "no terminal client views this pane; use load -d"};
  return *selected;
}
std::function<void()> load_handoff(Session session, std::optional<Client> caller) {
  std::optional<AttachCommand> command;
  if (!caller) {
    const auto prepared = session.attach_command();
    if (!prepared)
      throw Failure{1, "ATTACH_FAILED", prepared.error().diagnostic};
    command = *prepared;
  }
  return [session = std::move(session), caller = std::move(caller),
          command = std::move(command)] {
    if (caller) {
      const auto server = caller->server();
      if (!server)
        throw Failure{1, "CLIENT_CONTEXT", server.error().diagnostic};
      const auto current = current_client(*server, caller->active_pane_id());
      if (current.connection_identity() != caller->connection_identity() ||
          current.name() != caller->name() || current.pid() != caller->pid() ||
          current.created() != caller->created() || current.tty() != caller->tty())
        throw Failure{1, "CLIENT_CHANGED",
                      "the invoking client changed; loaded sessions remain"};
      // tmux targets a client name; it cannot atomically check this incarnation.
      const auto switched = current.switch_to(session);
      if (!switched)
        throw Failure{1, "SWITCH_FAILED", switched.error().diagnostic};
    } else {
      const auto child = run_child(
          command->argv(),
          {.terminal = true, .terminal_required = true, .timeout = std::nullopt});
      if (child.code != 0)
        throw Failure{child.code, "ATTACH_FAILED",
                      "attachment exited with status " + std::to_string(child.code) +
                          "; loaded sessions remain"};
    }
  };
}
struct Bootstrap {
  std::optional<Session> session;
  ~Bootstrap() {
    if (session)
      (void)session->kill();
  }
};
Server start_endpoint(const Request& request, Bootstrap& bootstrap) {
  std::vector<std::string> command{"tmux", "-u"};
  if (!request.value("S").empty())
    command.insert(command.end(), {"-S", expand(request.value("S"))});
  else if (!request.value("L").empty())
    command.insert(command.end(), {"-L", request.value("L")});
  else if (const auto inherited = environment("TMUX"); !inherited.empty()) {
    const auto last = inherited.find_last_of(',');
    const auto pid =
        last == std::string::npos ? last : inherited.find_last_of(',', last - 1);
    command.insert(command.end(), {"-S", inherited.substr(0, pid)});
  }
  if (!request.value("f").empty())
    command.insert(command.end(), {"-f", expand(request.value("f"))});
  if (request.flag("2"))
    command.emplace_back("-2");
  std::random_device random;
  std::ostringstream unique;
  unique << "tmux-workspace-bootstrap-" << std::hex;
  for (int index = 0; index < 4; ++index)
    unique << std::setw(8) << std::setfill('0') << random();
  const auto name = unique.str();
  command.insert(command.end(),
                 {"new-session", "-d", "-P", "-F", "#{session_id} #{pid}", "-s", name,
                  "--", "sleep 2147483647"});
  try {
    const auto reply = run_child(command);
    if (reply.code != 0)
      throw Failure{1, "STARTUP_FAILED", reply.err};
    std::string identity, pid, extra;
    std::istringstream response{reply.out};
    if (!(response >> identity >> pid) || (response >> extra) || identity.size() < 2 ||
        identity.front() != '$' ||
        identity.find_first_not_of("0123456789", 1) != std::string::npos ||
        pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos)
      throw Failure{1, "STARTUP_IDENTITY", "tmux returned an invalid startup identity"};
    const auto server = endpoint(request);
    const auto current_pid = server.run({"display-message", "-p", "#{pid}"});
    if (!current_pid || *current_pid != pid + "\n")
      throw Failure{1, "STARTUP_IDENTITY", "tmux server changed during startup"};
    const auto owned = server.session(identity);
    if (!owned || owned->name() != name)
      throw Failure{1, "STARTUP_IDENTITY",
                    "tmux bootstrap session changed during startup"};
    bootstrap.session = *owned;
    return server;
  } catch (Failure& error) {
    error.retained_state = {{"kind", "bootstrap-session"},
                            {"session_name", name},
                            {"ownership", "unverified"},
                            {"may_exist", true},
                            {"cleanup", "not_attempted"}};
    throw;
  }
}
Json capture(const Request& request) {
  const auto server = endpoint(request);
  const auto name = request.value("session");
  const bool by_id = name.size() > 1 && name.front() == '$' &&
                     name.find_first_not_of("0123456789", 1) == std::string::npos;
  const auto session = server.session(by_id ? name : "=" + name + ":");
  if (!session)
    throw Failure{1, "SESSION_NOT_FOUND", session.error().diagnostic};
  const auto windows = session->windows();
  if (!windows)
    throw Failure{1, "CAPTURE_FAILED", windows.error().diagnostic};
  Json document{{"session_name", session->name()}, {"windows", Json::array()}};
  for (const auto& window : *windows) {
    const auto panes = window.panes();
    if (!panes)
      throw Failure{1, "CAPTURE_FAILED", panes.error().diagnostic};
    Json item{{"window_name", window.name()},
              {"window_index", window.index()},
              {"layout", window.layout()},
              {"focus", window.active()},
              {"panes", Json::array()}};
    for (const auto& pane : *panes) {
      Json commands = Json::array();
      const std::string command{pane.command()};
      if (!command.empty() && command != "sh" && command != "bash" &&
          command != "zsh" && command != "fish")
        commands.push_back(command);
      item["panes"].push_back({{"shell_command", commands},
                               {"start_directory", pane.path()},
                               {"focus", pane.active()}});
    }
    document["windows"].push_back(item);
  }
  return document;
}
std::string yaml(const Json& document) {
  YAML::Emitter output;
  output << YAML::Load(document.dump());
  if (!output.good())
    throw Failure{1, "ENCODE_FAILED", output.GetLastError()};
  return std::string{output.c_str()} + "\n";
}
Json save_or_return(const Request& request, const Json& document, std::string format,
                    std::string destination = {}) {
  destination = request.value("save-to", destination);
  format = request.value("workspace-format", format);
  if (destination.empty() && request.machine()) {
    if (request.ndjson)
      return {{"schema_version", 1},
              {"command", request.command},
              {"status", "ok"},
              {"workspace", document}};
    return document;
  }
  if (destination.empty())
    throw Failure{2, "USAGE", "specify --save-to or a machine output mode"};
  const fs::path path = fs::absolute(expand(destination)).lexically_normal();
  if (fs::exists(path) && !request.flag("force"))
    throw Failure{1, "DESTINATION_EXISTS",
                  "destination exists; use --force: " + private_path(path)};
  auto temporary_name =
      (path.parent_path() / ("." + path.filename().string() + ".XXXXXX")).string();
  int descriptor = ::mkstemp(temporary_name.data());
  if (descriptor < 0)
    throw Failure{1, "WRITE_FAILED", "cannot create a temporary output file"};
  const fs::path temporary{temporary_name};
  try {
    const auto bytes = format == "json" ? encoded(document, 2) + "\n" : yaml(document);
    std::size_t offset{};
    while (offset < bytes.size()) {
      const auto count =
          ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0)
        throw Failure{1, "WRITE_FAILED", "could not write destination"};
      offset += static_cast<std::size_t>(count);
    }
    const int closed = ::close(descriptor);
    descriptor = -1;
    if (closed != 0)
      throw Failure{1, "WRITE_FAILED", "could not close destination"};
    if (request.flag("force"))
      fs::rename(temporary, path);
    else {
      fs::create_hard_link(temporary, path);
      fs::remove(temporary);
    }
  } catch (...) {
    if (descriptor >= 0)
      ::close(descriptor);
    std::error_code ignored;
    fs::remove(temporary, ignored);
    throw;
  }
  return {{"schema_version", 1},
          {"command", request.command},
          {"status", "ok"},
          {"destination", private_path(path)},
          {"format", format}};
}
Json imported(Json source, const std::string& kind) {
  if (kind == "teamocil" && source.contains("session"))
    source = source["session"];
  Json result{
      {"session_name", source.value("project_name", source.value("name", Json{}))},
      {"windows", Json::array()}};
  if (source.contains("project_root"))
    result["start_directory"] = source["project_root"];
  else if (source.contains("root"))
    result["start_directory"] = source["root"];
  if (source.contains("pre_window"))
    result["shell_command_before"] = source["pre_window"];
  else if (source.contains("pre"))
    result["shell_command_before"] = source["pre"];
  const auto windows = source.value("tabs", source.value("windows", Json::array()));
  if (!windows.is_array())
    throw Failure{1, "INVALID_CONFIG", "imported windows must be an array"};
  for (const auto& raw : windows) {
    if (!raw.is_object())
      throw Failure{1, "INVALID_CONFIG", "imported window must be a mapping"};
    const auto append = [&](const std::string& name, Json body) {
      Json window{{"window_name", name}};
      if (!body.is_object())
        window["panes"] = body.is_array() ? body : Json::array({body});
      else {
        for (const auto& [from, to] : {std::pair{"root", "start_directory"},
                                       {"layout", "layout"},
                                       {"pre", "shell_command_before"},
                                       {"panes", "panes"},
                                       {"splits", "panes"}})
          if (body.contains(from))
            window[to] = body[from];
        if (window.contains("panes"))
          for (auto& pane : window["panes"]) {
            if (pane.is_object() && pane.contains("cmd")) {
              pane["shell_command"] = pane["cmd"];
              pane.erase("cmd");
            }
          }
      }
      result["windows"].push_back(window);
    };
    if (kind == "teamocil")
      append(raw.value("name", ""), raw);
    else
      for (auto it = raw.begin(); it != raw.end(); ++it)
        append(it.key(), it.value());
  }
  return result;
}
struct Pattern {
  std::vector<std::string> fields;
  std::regex regex;
};
std::string field_name(std::string value) {
  static const std::map<std::string, std::string> aliases{
      {"n", "name"},
      {"name", "name"},
      {"s", "session_name"},
      {"session", "session_name"},
      {"session_name", "session_name"},
      {"p", "path"},
      {"path", "path"},
      {"w", "window"},
      {"window", "window"},
      {"pane", "pane"},
      {"cmd", "pane"}};
  const auto found = aliases.find(value);
  if (found == aliases.end())
    throw Failure{2, "USAGE", "unknown search field: " + value};
  return found->second;
}
std::vector<Pattern> patterns(const Request& request) {
  std::vector<std::string> defaults;
  for (const auto& field : request.list("field"))
    defaults.push_back(field_name(field));
  if (defaults.empty())
    defaults = {"name", "session_name", "path", "window", "pane"};
  std::vector<Pattern> result;
  for (auto query : request.list("query")) {
    auto fields = defaults;
    if (const auto colon = query.find(':'); colon != std::string::npos) {
      try {
        fields = {field_name(query.substr(0, colon))};
        query.erase(0, colon + 1);
      } catch (const Failure&) {
      }
    }
    const bool uppercase = std::ranges::any_of(
        query, [](unsigned char c) { return std::isupper(c) != 0; });
    if (request.flag("fixed-strings"))
      query = std::regex_replace(query, std::regex{R"([.^$|()\[\]{}*+?\\])"}, R"(\$&)");
    if (request.flag("word-regexp"))
      query = "\\b(?:" + query + ")\\b";
    auto flags = std::regex::ECMAScript;
    if (request.flag("ignore-case") || (request.flag("smart-case") && !uppercase))
      flags |= std::regex::icase;
    try {
      result.push_back({fields, std::regex{query, flags}});
    } catch (const std::regex_error& error) {
      throw Failure{2, "INVALID_PATTERN", error.what()};
    }
  }
  return result;
}
Json search(const Request& request) {
  const auto compiled = patterns(request);
  Json results = Json::array();
  for (const auto& file : discover()) {
    Json info = record(file, true);
    std::map<std::string, std::vector<std::string>> fields;
    for (const auto* key : {"name", "session_name", "path"})
      if (info[key].is_string())
        fields[key].push_back(info[key].get<std::string>());
    const auto config = info["config"];
    if (config.is_object() && config.contains("windows") &&
        config["windows"].is_array()) {
      for (const auto& window : config["windows"]) {
        if (!window.is_object())
          continue;
        if (window.contains("window_name") && window["window_name"].is_string())
          fields["window"].push_back(window["window_name"].get<std::string>());
        if (window.contains("panes"))
          for (const auto& pane : window["panes"]) {
            if (pane.is_string())
              fields["pane"].push_back(pane.get<std::string>());
            else if (pane.is_object() && pane.contains("shell_command"))
              fields["pane"].push_back(pane["shell_command"].is_string()
                                           ? pane["shell_command"].get<std::string>()
                                           : pane["shell_command"].dump());
            else if (pane.is_array())
              for (const auto& command : pane)
                if (command.is_string())
                  fields["pane"].push_back(command.get<std::string>());
          }
      }
    }
    Json matches = Json::object();
    std::size_t matched{};
    for (const auto& pattern : compiled) {
      bool found{};
      for (const auto& field : pattern.fields)
        for (const auto& value : fields[field]) {
          if (std::regex_search(value, pattern.regex)) {
            found = true;
            if (!matches.contains(field))
              matches[field] = Json::array();
            if (std::find(matches[field].begin(), matches[field].end(), Json(value)) ==
                matches[field].end())
              matches[field].push_back(value);
          }
        }
      if (found)
        ++matched;
    }
    bool selected = request.flag("any") ? matched != 0 : matched == compiled.size();
    if (request.flag("invert-match"))
      selected = !selected;
    if (!selected)
      continue;
    Json names = Json::array();
    for (auto it = matches.begin(); it != matches.end(); ++it)
      names.push_back(it.key());
    results.push_back({{"name", info["name"]},
                       {"path", info["path"]},
                       {"session_name", info["session_name"]},
                       {"source", info["source"]},
                       {"matched_fields", names},
                       {"matches", matches}});
  }
  return results;
}
} // namespace
std::string encoded(const Json& value, int indent) {
  return value.dump(indent, ' ', false, Json::error_handler_t::replace);
}
void validate(const Request& request) {
  if (request.command == "load" && request.flag("8"))
    throw Failure{2, "USAGE", "88-colour mode is unsupported; use -2 for 256 colours"};
  if (request.command == "search") {
    if (request.list("query").empty())
      throw Failure{2, "USAGE", "search requires at least one pattern"};
    (void)patterns(request);
  }
  if (request.command == "load" && !request.flag("d")) {
    if (!request.flag("append")) {
      if (request.machine())
        throw Failure{2, "USAGE", "machine output requires load -d or --append"};
      if (!request.terminal_allowed)
        throw Failure{2, "USAGE",
                      "load requires a foreground controlling terminal; use -d"};
      require_terminal();
    }
    if (request.flag("append") || !environment("TMUX").empty()) {
      const auto pane = environment("TMUX_PANE");
      const auto context = environment("TMUX");
      const auto last = context.find_last_of(',');
      const auto previous = last == std::string::npos || last == 0
                                ? std::string::npos
                                : context.find_last_of(',', last - 1);
      if (previous == std::string::npos || previous == 0 || last == previous + 1 ||
          last + 1 == context.size() ||
          context.find_first_not_of("0123456789", previous + 1) != last ||
          context.find_first_not_of("0123456789", last + 1) != std::string::npos ||
          pane.size() < 2 || pane.front() != '%' ||
          pane.find_first_not_of("0123456789", 1) != std::string::npos)
        throw Failure{2, "USAGE", "load requires valid TMUX and TMUX_PANE context"};
    }
  }
  if (request.command == "freeze" && request.value("session").empty())
    throw Failure{2, "USAGE", "freeze requires a session name or ID"};
  if (request.command == "shell")
    throw Failure{1, "FEATURE_UNAVAILABLE",
                  "process services are not implemented in this build"};
}
Execution execute(const Request& request, const EventSink& event) {
  if (request.command == "edit") {
    const auto path = resolve(request.value("workspace-file"));
    const auto visual = environment("VISUAL"), editor = environment("EDITOR");
    auto arguments = split_command(!visual.empty()   ? visual
                                   : !editor.empty() ? editor
                                                     : "vi");
    arguments.push_back(path.string());
    event("started", {{"path", private_path(path)}});
    try {
      const auto child = run_child(
          arguments, {.terminal = request.terminal_allowed, .timeout = std::nullopt});
      return {.value = {{"schema_version", 1},
                        {"command", "edit"},
                        {"path", private_path(path)},
                        {"exit_code", child.code},
                        {"stdout", child.out},
                        {"stderr", child.err},
                        {"status", child.code == 0 ? "ok" : "error"}}};
    } catch (const Failure& error) {
      event("failed",
            {{"status", "error"},
             {"exit_code", error.exit_code},
             {"errors",
              Json::array({{{"code", error.code}, {"message", error.what()}}})}});
      throw;
    }
  }

  if (request.command == "ls") {
    Json workspaces = Json::array(), dirs = Json::array();
    for (const auto& file : discover())
      workspaces.push_back(record(file, request.flag("full")));
    for (const auto& directory : global_directories())
      dirs.push_back(
          {{"path", private_path(directory)}, {"exists", fs::is_directory(directory)}});
    return {.value = {{"workspaces", workspaces}, {"global_workspace_dirs", dirs}}};
  }
  if (request.command == "search")
    return {.value = search(request)};
  if (request.command == "debug-info") {
    return {.value = {{"port", "cxx"},
                      {"version", LIBTMUX_WORKSPACE_VERSION},
                      {"compiler", __VERSION__},
                      {"cwd", private_path(fs::current_path())},
                      {"home", "~"},
                      {"regex", "ECMAScript"}}};
  }
  if (request.command == "convert" || request.command == "import") {
    const auto path = resolve(request.value("workspace-file"), request.importer);
    auto document = read_document(path);
    if (request.command == "import")
      document = imported(document, request.importer);
    const auto format = path.extension() == ".json" ? "yaml" : "json";
    if (!request.machine() && !request.flag("yes") && !request.flag("save-to")) {
      return {.value = {{"preview", true},
                        {"format", request.value("workspace-format", format)},
                        {"workspace", document}}};
    }
    auto destination = path;
    destination.replace_extension(format == std::string{"json"} ? ".json" : ".yaml");
    return {.value = save_or_return(request, document, format,
                                    request.machine() ? "" : destination.string())};
  }
  if (request.command == "freeze") {
    const auto document = capture(request);
    auto result = save_or_return(request, document, "yaml");
    if (request.ndjson || result.contains("destination"))
      result["warnings"] =
          Json::array({"Capture cannot recover original command arguments, history, "
                       "plugins or before scripts."});
    return {.value = std::move(result)};
  }
  if (request.command == "load") {
    struct Planned {
      fs::path path;
      Workspace workspace;
      std::vector<std::string> before_script;
      std::string script_directory;
    };
    std::vector<Planned> plans;
    const auto inputs = request.list("workspace-file");
    for (std::size_t index = 0; index < inputs.size(); ++index) {
      const auto& input = inputs[index];
      const auto path = resolve(input);
      auto document = read_document(path);
      normalise(document, path.parent_path());
      std::vector<std::string> script;
      if (document.contains("before_script") && !document["before_script"].is_null()) {
        if (!document["before_script"].is_string())
          throw Failure{1, "INVALID_CONFIG", "before_script must be a command string"};
        const auto command = document["before_script"].get<std::string>();
        if (!command.empty()) {
          try {
            script = split_command(command);
          } catch (const Failure& error) {
            throw Failure{1, "INVALID_CONFIG",
                          "before_script: " + std::string{error.what()}};
          }
          for (const auto& argument : script)
            if (argument.find('\0') != std::string::npos)
              throw Failure{1, "INVALID_CONFIG",
                            "before_script arguments cannot contain NUL"};
        }
      }
      document.erase("before_script");
      if (index + 1 == inputs.size() && !request.value("s").empty())
        document["session_name"] = request.value("s");
      const auto workspace = parse_tmuxp(document.dump());
      if (!workspace)
        throw Failure{1, "INVALID_CONFIG",
                      workspace.error().where + ": " + workspace.error().reason};
      if (!libtmux::session_target(workspace->session_name))
        throw Failure{1, "INVALID_CONFIG", "session name cannot address itself"};
      std::string directory;
      if (!script.empty()) {
        directory = workspace->start_directory.empty() ? fs::current_path().string()
                                                       : workspace->start_directory;
        if (directory.find('\0') != std::string::npos || !fs::is_directory(directory))
          throw Failure{1, "INVALID_CONFIG",
                        "before_script working directory does not exist"};
      }
      plans.push_back({path, *workspace, std::move(script), std::move(directory)});
    }
    Json results = Json::array(), errors = Json::array();
    event("started", {{"inputs", plans.size()}});
    Bootstrap bootstrap;
    const bool appending = request.flag("append") && !request.flag("d");
    bool retained_changes{};
    int failure_status{1};
    const bool interactive = !request.flag("d") && !appending;
    std::optional<Session> selected;
    std::optional<Client> caller;
    std::string stage{"startup"};
    std::size_t active_input{};
    try {
      auto server = endpoint(request);
      std::optional<Session> borrowed;
      if (appending)
        borrowed = append_target(server);
      else if (interactive && !environment("TMUX").empty()) {
        const auto pane = current_pane(server, "CLIENT_CONTEXT");
        caller = current_client(server, pane.id());
      } else if (!server.is_alive())
        server = start_endpoint(request, bootstrap);
      stage = "load";
      for (std::size_t index = 0; index < plans.size(); ++index) {
        active_input = index;
        const auto& plan = plans[index];
        event("workspace-started",
              {{"input_index", index}, {"input", private_path(plan.path)}});
        const auto existing =
            borrowed ? libtmux::expected<Session, CommandFailure>{*borrowed}
                     : server.session("=" + plan.workspace.session_name + ":");
        Json script_output;
        std::optional<Failure> script_error;
        BeforeBuild before;
        if (!plan.before_script.empty()) {
          before = [&](const Session& session) -> std::optional<std::string> {
            stage = "before-script";
            try {
              event("script-started",
                    {{"input_index", index}, {"session_id", session.id()}});
              const auto child = run_child(
                  plan.before_script,
                  {.terminate_descendants = true,
                   .timeout = std::nullopt,
                   .directory = plan.script_directory,
                   .output = [&](std::string_view stream, std::string_view bytes) {
                     event(
                         "script-output",
                         {{"input_index", index}, {"stream", stream}, {"text", bytes}});
                   }});
              script_output = child.value();
              event("script-completed",
                    {{"input_index", index}, {"script_output", script_output}});
              if (child.code != 0) {
                script_error.emplace(
                    child.code >= 128 ? child.code : 1, "BEFORE_SCRIPT_FAILED",
                    "before_script exited with status " + std::to_string(child.code));
                return script_error->what();
              }
            } catch (const Failure& error) {
              script_error = error;
              script_output = error.child_output;
              return error.what();
            }
            stage = "load";
            return std::nullopt;
          };
        }
        auto built = borrowed   ? append(*borrowed, plan.workspace, before)
                     : existing ? libtmux::expected<Session, BuildError>{*existing}
                                : build(server, plan.workspace, before);
        if (!built) {
          if (script_error && script_error->code == "OUTPUT_CLOSED")
            throw *script_error;
          if (script_error)
            failure_status = script_error->exit_code;
          Json problem{{"code", script_error ? script_error->code : "BUILD_FAILED"},
                       {"message", built.error().reason},
                       {"input_index", index},
                       {"failed_stage", stage}};
          if (!script_output.is_null())
            problem["script_output"] = script_output;
          if (borrowed) {
            retained_changes = true;
            problem["retained_state"] = {{"session_id", borrowed->id()},
                                         {"session_name", borrowed->name()},
                                         {"ownership", "borrowed"},
                                         {"window_ids", built.error().retained_windows},
                                         {"settings_may_have_changed", true}};
          }
          errors.push_back(std::move(problem));
          break;
        }
        Json result{{"input", private_path(plan.path)},
                    {"input_index", index},
                    {"session_id", built->id()},
                    {"session_name", built->name()},
                    {"action", borrowed   ? "appended"
                               : existing ? "reused"
                                          : "created"}};
        if (!script_output.is_null())
          result["script_output"] = script_output;
        results.push_back(result);
        if (index + 1 == plans.size())
          selected = *built;
        if (!existing)
          event("session-created", result);
        event("workspace-completed", result);
      }
    } catch (const Failure& error) {
      if (error.code == "OUTPUT_CLOSED")
        throw;
      failure_status = error.exit_code;
      Json problem{{"code", error.code},
                   {"message", error.what()},
                   {"input_index", active_input},
                   {"failed_stage", stage}};
      if (!error.retained_state.is_null())
        problem["retained_state"] = error.retained_state;
      errors.push_back(std::move(problem));
    } catch (const std::exception& error) {
      errors.push_back({{"code", "OPERATION_FAILED"},
                        {"message", error.what()},
                        {"input_index", active_input},
                        {"failed_stage", stage}});
    }
    if (bootstrap.session) {
      const auto cleaned = bootstrap.session->kill();
      bootstrap.session.reset();
      if (!cleaned)
        errors.push_back({{"code", "BOOTSTRAP_CLEANUP_FAILED"},
                          {"message", cleaned.error().diagnostic},
                          {"failed_stage", "cleanup"}});
    }
    const auto status = errors.empty()                         ? "ok"
                        : results.empty() && !retained_changes ? "error"
                                                               : "partial";
    Json summary{
        {"schema_version", 1}, {"command", "load"},
        {"status", status},    {"exit_code", errors.empty() ? 0 : failure_status},
        {"results", results},  {"errors", errors}};
    Execution execution{.value = std::move(summary)};
    if (interactive && errors.empty() && selected) {
      try {
        execution.handoff = load_handoff(*selected, std::move(caller));
      } catch (Failure& error) {
        error.retained_state = std::move(execution.value);
        throw;
      }
    }
    return execution;
  }
  throw Failure{1, "FEATURE_UNAVAILABLE", "command is not implemented"};
}
std::string human_result(const Request& request, const Json& result, bool colour) {
  const auto role = [colour](const std::string& code, const std::string& value) {
    return colour ? "\033[" + code + "m" + value + "\033[0m" : value;
  };
  std::ostringstream output;
  if (result.is_object() && result.value("preview", false)) {
    return result.at("format") == "json" ? encoded(result.at("workspace"), 2) + "\n"
                                         : yaml(result.at("workspace"));
  }
  if (request.command == "edit")
    return result.at("stdout").get<std::string>();
  if (request.command == "ls" || request.command == "search") {
    const auto& rows = request.command == "ls" ? result.at("workspaces") : result;
    for (const auto& row : rows)
      output << role("1;35", row.at("name").get<std::string>()) << "  "
             << role("36", row.at("path").get<std::string>()) << '\n';
  } else if (request.command == "load") {
    for (const auto& item : result.at("results"))
      output << role("32", item.at("action").get<std::string>()) << ' '
             << role("1;35", item.at("session_name").get<std::string>()) << ' '
             << role("2", item.at("session_id").get<std::string>()) << '\n';
  } else if (result.contains("destination")) {
    if (!request.flag("quiet"))
      output << role("32", "Saved") << ' '
             << role("36", result.at("destination").get<std::string>()) << '\n';
  } else
    output << encoded(result, 2) << '\n';
  return output.str();
}
} // namespace libtmux::workspace::cli
