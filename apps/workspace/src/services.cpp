#include "services.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <random>
#include <ranges>
#include <regex>
#include <set>
#include <sstream>
#include <sys/ioctl.h>
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
// A positive whole number from the environment, or nullopt when unset.
// Anything else (letters, zero, negative, fractional) is a usage error here,
// where tmuxp's own `shutil.get_terminal_size` silently treats a bad
// COLUMNS/LINES as absent.
std::optional<int> sized_env(const char* name) {
  const auto value = environment(name);
  if (value.empty())
    return std::nullopt;
  long long parsed{};
  const auto* end = value.data() + value.size();
  const auto result = std::from_chars(value.data(), end, parsed);
  if (result.ec != std::errc{} || result.ptr != end || parsed <= 0)
    throw Failure{2, "usage", std::string{name} + " must be a positive whole number"};
  return static_cast<int>(parsed);
}
// The size a newly built session is given while no client is attached,
// matching tmuxp's own `shutil.get_terminal_size(fallback=(columns, rows))`
// so cxx and `uvx tmuxp` build identical layouts from the same terminal.
// nullopt/nullopt means "pass no -x/-y", which is what a disabled detection
// and tmux's own `default-size` mean.
std::pair<std::optional<int>, std::optional<int>>
session_dimensions(bool stdout_terminal) {
  const auto detect = environment("TMUXP_DETECT_TERMINAL_SIZE");
  if (!detect.empty() && detect != "1")
    return {std::nullopt, std::nullopt};
  auto columns = sized_env("COLUMNS");
  auto rows = sized_env("LINES");
  if (!columns || !rows) {
    struct winsize size {};
    const bool have_terminal = stdout_terminal &&
                               ::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 &&
                               size.ws_col > 0 && size.ws_row > 0;
    if (have_terminal) {
      if (!columns)
        columns = size.ws_col;
      if (!rows)
        rows = size.ws_row;
    } else {
      if (!columns)
        columns = sized_env("TMUXP_DEFAULT_COLUMNS").value_or(80);
      if (!rows)
        rows = sized_env("TMUXP_DEFAULT_ROWS").value_or(24);
    }
  }
  return {columns, rows};
}
std::string private_path(const fs::path& path) {
  auto value = path.string();
  const auto home = environment("HOME");
  if (!home.empty() && (value == home || value.starts_with(home + "/")))
    value.replace(0, home.size(), "~");
  return value;
}
std::string visible_controls(std::string_view text) {
  constexpr std::string_view hex = "0123456789abcdef";
  std::string result;
  for (std::size_t index = 0; index < text.size(); ++index) {
    auto byte = static_cast<unsigned char>(text[index]);
    if (byte == 0xc2 && index + 1 < text.size() &&
        static_cast<unsigned char>(text[index + 1]) >= 0x80 &&
        static_cast<unsigned char>(text[index + 1]) <= 0x9f)
      byte = static_cast<unsigned char>(text[++index]);
    else if (byte != 0x7f) {
      result += text[index];
      continue;
    }
    result += "\\u00";
    result += hex[byte >> 4];
    result += hex[byte & 0xf];
  }
  return result;
}
std::string human_text(const std::string& value) {
  const auto quoted = encoded(Json(value));
  return visible_controls(std::string_view{quoted}.substr(1, quoted.size() - 2));
}
// A document-level ParseError has no path (`where` is empty); joining it
// with ": " regardless produced "Error: : unsupported key: ...".
std::string parse_error_message(const workspace::ParseError& error) {
  return error.where.empty() ? error.reason : error.where + ": " + error.reason;
}
// A refused key is its own `unsupported_key` code, not the general
// `invalid_workspace` every other malformed document gets.
const char* parse_error_code(const workspace::ParseError& error) {
  return error.unsupported_key ? "unsupported_key" : "invalid_workspace";
}
Json from_yaml(const YAML::Node& node, int depth = 0) {
  if (depth > 64)
    throw Failure{1, "invalid_workspace", "workspace nesting exceeds 64 levels"};
  if (!node || node.IsNull())
    return nullptr;
  if (node.IsSequence()) {
    Json array = Json::array();
    for (const auto& child : node)
      array.push_back(from_yaml(child, depth + 1));
    return array;
  }
  if (node.IsMap()) {
    // `<<: *anchor` or `<<: [*a, *b]` merges that mapping's (or those
    // mappings', earlier winning) keys into this one; an explicit key here
    // always overrides a merged one. A quoted `"<<"` is an ordinary key, not
    // a merge directive.
    Json object = Json::object();
    std::vector<std::pair<std::string, YAML::Node>> explicit_entries;
    std::vector<YAML::Node> merge_sources;
    std::set<std::string> seen;
    for (const auto& entry : node) {
      if (!entry.first.IsScalar())
        throw Failure{1, "invalid_workspace", "mapping keys must be strings"};
      const auto key = entry.first.Scalar();
      const bool literal =
          entry.first.Tag() == "!" || entry.first.Tag() == "tag:yaml.org,2002:str";
      if (key == "<<" && !literal) {
        if (entry.second.IsSequence())
          for (const auto& source : entry.second)
            merge_sources.push_back(source);
        else
          merge_sources.push_back(entry.second);
        continue;
      }
      if (!seen.insert(key).second)
        throw Failure{1, "invalid_workspace", "duplicate mapping key: " + key};
      explicit_entries.emplace_back(key, entry.second);
    }
    for (const auto& source : merge_sources) {
      if (!source.IsMap())
        throw Failure{1, "invalid_workspace",
                      "<< merges a mapping or a list of mappings"};
      // Resolve the source itself first: a merge source that is itself
      // merging (chained defaults) is ordinary YAML, and its own literal
      // "<<" must not leak into this mapping's result.
      const Json resolved = from_yaml(source, depth + 1);
      if (!resolved.is_object())
        throw Failure{1, "invalid_workspace",
                      "<< merges a mapping or a list of mappings"};
      for (const auto& [key, value] : resolved.items())
        if (!object.contains(key))
          object[key] = value;
    }
    for (const auto& [key, value] : explicit_entries)
      object[key] = from_yaml(value, depth + 1);
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
Json read_document(const fs::path& path, bool reject_erb = false) {
  std::ifstream file{path, std::ios::binary};
  if (!file)
    throw Failure{1, "invalid_workspace", "cannot read " + private_path(path)};
  std::string contents;
  char buffer[8192];
  while (file.read(buffer, sizeof buffer) || file.gcount() != 0) {
    contents.append(buffer, static_cast<std::size_t>(file.gcount()));
    if (contents.size() > 4U * 1024U * 1024U)
      throw Failure{1, "invalid_workspace", "workspace exceeds 4 MiB"};
  }
  if (reject_erb && contents.find("<%") != std::string::npos)
    throw Failure{1, "invalid_workspace", "tmuxinator ERB templates are unsupported"};
  Json result;
  try {
    if (path.extension() == ".json")
      result = Json::parse(contents);
    else {
      const auto documents = YAML::LoadAll(contents);
      if (documents.size() != 1)
        throw Failure{1, "invalid_workspace", "exactly one YAML document is required"};
      result = from_yaml(documents.front());
    }
  } catch (const YAML::Exception& error) {
    throw Failure{1, "invalid_workspace", error.what()};
  } catch (const Json::parse_error& error) {
    throw Failure{1, "invalid_workspace", error.what()};
  }
  if (!result.is_object())
    throw Failure{1, "invalid_workspace", "workspace must be a mapping"};
  return result;
}
// tmuxp has exactly one active global workspace directory: the first that
// exists of $TMUXP_CONFIGDIR, $XDG_CONFIG_HOME/tmuxp (else ~/.config/tmuxp),
// then legacy ~/.tmuxp. Falls back to the last candidate when none exist.
std::vector<fs::path> global_directories() {
  std::vector<fs::path> candidates;
  const auto explicit_dir = environment("TMUXP_CONFIGDIR");
  if (!explicit_dir.empty())
    candidates.emplace_back(expand(explicit_dir));
  const auto home = environment("HOME");
  auto xdg = environment("XDG_CONFIG_HOME");
  if (xdg.empty() && !home.empty())
    xdg = (fs::path{home} / ".config").string();
  if (!xdg.empty())
    candidates.emplace_back(fs::path{xdg} / "tmuxp");
  if (!home.empty())
    candidates.emplace_back(fs::path{home} / ".tmuxp");
  for (const auto& dir : candidates) {
    std::error_code error;
    if (fs::is_directory(dir, error))
      return {fs::absolute(dir).lexically_normal()};
  }
  if (!candidates.empty())
    return {fs::absolute(candidates.back()).lexically_normal()};
  return {};
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
  throw Failure{1, "workspace_not_found", "workspace not found: " + name};
}
// Empty for a file that went away between the listing and this read, which
// `discover` cannot rule out and a listing should not end on.
std::optional<Json> record(const WorkspaceFile& file, bool full) {
  std::error_code gone;
  const auto modified = fs::last_write_time(file.path, gone);
  if (gone)
    return std::nullopt;
  const auto size = fs::file_size(file.path, gone);
  if (gone)
    return std::nullopt;
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
              {"size", size},
              {"mtime", time.str()},
              {"session_name",
               config.is_object() ? config.value("session_name", Json{}) : Json{}},
              {"source", file.source}};
  if (full)
    result["config"] = config;
  return result;
}
// Expand $VAR, ${VAR} and a leading ~ in command text from the loading
// process's environment, matching tmuxp. Handles every shape a command list
// can take: a bare string, a list of strings, a list of {cmd: ...} mappings,
// or (shell_command_before, after tmuxp's own normalisation) a mapping
// wrapping one of those under "shell_command".
void expand_commands(Json& value) {
  if (value.is_string()) {
    value = expand(value.get<std::string>());
    return;
  }
  if (value.is_object() && value.contains("shell_command")) {
    expand_commands(value["shell_command"]);
    return;
  }
  if (!value.is_array())
    return;
  for (auto& item : value) {
    if (item.is_string())
      item = expand(item.get<std::string>());
    else if (item.is_object() && item.value("cmd", Json{}).is_string())
      item["cmd"] = expand(item["cmd"].get<std::string>());
  }
}
void normalise(Json& node, const fs::path& base, const fs::path& parent = {}) {
  if (!node.is_object())
    return;
  for (const auto* key : {"session_name", "window_name"})
    if (node.contains(key) && node[key].is_string())
      node[key] = expand(node[key].get<std::string>());
  if (node.contains("before_script") && node["before_script"].is_string())
    node["before_script"] = expand(node["before_script"].get<std::string>());
  for (const auto* key : {"shell_command", "shell_command_before"})
    if (node.contains(key))
      expand_commands(node[key]);
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
    // A pane may be a bare command string rather than a mapping; normalise()
    // only recurses into mappings, so expand it here directly.
    for (auto& child : node[key]) {
      if (std::string_view{key} == "panes" && child.is_string())
        child = expand(child.get<std::string>());
      else
        normalise(child, base, directory);
    }
  }
}
// The one reading of TMUX in this tool: `socket,pid,session`, where a socket
// path may itself contain a comma, so the split is at the last comma that can
// begin the pid. A value of another shape is that variable's problem, not a
// different server and not a missing terminal.
struct TmuxContext {
  std::string socket, pid, session;
};
TmuxContext tmux_context() {
  const auto refuse = [] {
    return Failure{2, "usage",
                   "TMUX does not name a tmux client context; it is written "
                   "socket,pid,session"};
  };
  const auto value = environment("TMUX");
  const auto last = value.find_last_of(',');
  const auto previous = last == std::string::npos || last == 0
                            ? std::string::npos
                            : value.find_last_of(',', last - 1);
  if (previous == std::string::npos || previous == 0)
    throw refuse();
  const TmuxContext context{value.substr(0, previous),
                            value.substr(previous + 1, last - previous - 1),
                            value.substr(last + 1)};
  const auto digits = [](std::string_view text) {
    return !text.empty() &&
           text.find_first_not_of("0123456789") == std::string_view::npos;
  };
  if (!digits(context.pid) || !digits(context.session))
    throw refuse();
  return context;
}
bool same_socket(std::string_view left, std::string_view right) {
  if (left == right)
    return true;
  std::error_code ignored;
  return fs::weakly_canonical(fs::path{left}, ignored) ==
         fs::weakly_canonical(fs::path{right}, ignored);
}
// Every start_directory the document names that is not a directory, with
// where it was written. tmux starts such a pane in $HOME instead, silently,
// so a typo would otherwise cost a workspace that looks built and is not.
std::vector<std::pair<std::string, std::string>>
absent_directories(const Workspace& description) {
  std::vector<std::pair<std::string, std::string>> absent;
  const auto check = [&absent](const std::string& where, const std::string& path) {
    if (path.empty() || path.find('\0') != std::string::npos)
      return;
    std::error_code ignored;
    if (!fs::is_directory(path, ignored))
      absent.emplace_back(where, path);
  };
  check("start_directory", description.start_directory);
  for (std::size_t window = 0; window < description.windows.size(); ++window) {
    const auto where = "windows[" + std::to_string(window) + "]";
    check(where + ".start_directory", description.windows[window].start_directory);
    const auto& panes = description.windows[window].panes;
    for (std::size_t pane = 0; pane < panes.size(); ++pane)
      check(where + ".panes[" + std::to_string(pane) + "].start_directory",
            panes[pane].start_directory);
  }
  return absent;
}
// The document's windows that a session already running under that name does
// not have. Empty means the session holds everything the document asks for;
// std::nullopt means its windows could not be read. A window the document
// leaves unnamed is not compared: tmux names it after whatever it runs.
std::optional<std::vector<std::string>> absent_windows(const Session& session,
                                                       const Workspace& description) {
  const auto present = session.windows();
  if (!present)
    return std::nullopt;
  std::multiset<std::string> live;
  for (const auto& window : *present)
    live.emplace(window.name());
  std::vector<std::string> absent;
  for (const auto& window : description.windows) {
    if (window.name.empty())
      continue;
    if (const auto found = live.find(window.name); found != live.end())
      live.erase(found);
    else
      absent.push_back(window.name);
  }
  return absent;
}
Server endpoint(const Request& request) {
  auto server =
      !request.value("S").empty()   ? Server::at_socket_path(expand(request.value("S")))
      : !request.value("L").empty() ? Server::at_socket_name(request.value("L"))
      : !environment("TMUX").empty() ? Server::at_socket_path(tmux_context().socket)
                                     : Server::at_default();
  if (!server)
    throw Failure{2, "usage", "the selected tmux socket cannot be used"};
  return *server;
}
// Resolves the context an attached load hands off through, before anything is
// built: TMUX parses, names the server this command targets and a process
// still running it, and TMUX_PANE is a pane there with a terminal.
libtmux::Pane current_pane(const Server& server) {
  const auto context = tmux_context();
  if (!same_socket(context.socket, server.socket_path()))
    throw Failure{2, "usage",
                  "selected server differs from the current pane's server; use -d"};
  const auto identifier = environment("TMUX_PANE");
  if (identifier.size() < 2 || identifier.front() != '%' ||
      identifier.find_first_not_of("0123456789", 1) != std::string::npos)
    throw Failure{2, "usage", "TMUX_PANE is not a pane id: " + identifier};
  const auto selected = server.pane(identifier);
  if (!selected)
    throw Failure{2, "usage",
                  "this tmux server has no pane " + identifier + "; use -d"};
  const auto pid = selected->expand("#{pid}");
  if (!pid || *pid != context.pid)
    throw Failure{2, "usage",
                  "TMUX names a tmux server that is no longer running on this "
                  "socket; use -d"};
  if (selected->tty().empty())
    throw Failure{2, "usage", "the current pane has no terminal; use -d"};
  return *selected;
}
Session append_target(const Server& server) {
  const auto session = current_pane(server).session();
  if (!session)
    throw Failure{2, "usage", "the current pane names no session; use -d"};
  return *session;
}
// `resolving` separates the two callers: resolving the context before
// anything is built is a refusal about how the command was invoked, while the
// same lookup repeated at hand-off time reports a server that changed under a
// load whose sessions already exist.
Client current_client(const Server& server, std::string_view pane,
                      std::string_view window, bool resolving = true) {
  const auto refuse = [resolving](std::string message) {
    return Failure{resolving ? 2 : 1, resolving ? "usage" : "tmux_failed",
                   std::move(message)};
  };
  const auto clients = server.clients();
  if (!clients)
    throw refuse("this tmux server did not report its clients; use load -d");
  std::optional<Client> selected;
  for (const auto& client : *clients) {
    if (client.control_mode() || client.tty().empty() || client.window_id() != window)
      continue;
    if (std::ranges::any_of(client.flags() | std::views::split(','), [](auto flag) {
          return std::ranges::equal(flag, std::string_view{"active-pane"});
        }))
      throw refuse("independent active-pane client focus is unverifiable; use load -d");
    if (client.active_pane_id() != pane)
      continue;
    if (selected)
      throw refuse("multiple clients view this pane; use load -d");
    selected = client;
  }
  if (!selected || selected->pid() <= 0 || selected->name().empty())
    throw refuse("no terminal client views this pane; use load -d");
  return *selected;
}
std::function<void()> load_handoff(Session session, std::optional<Client> caller,
                                   bool switch_without_client = false) {
  std::optional<AttachCommand> command;
  if (!caller && !switch_without_client) {
    const auto prepared = session.attach_command();
    if (!prepared)
      throw Failure{1, "tmux_failed",
                    "tmux could not prepare the attachment; the loaded sessions "
                    "remain"};
    command = *prepared;
  }
  return [session = std::move(session), caller = std::move(caller),
          command = std::move(command), switch_without_client] {
    const auto unreachable = [] {
      return Failure{1, "tmux_failed",
                     "the tmux server could not be reached; the loaded sessions "
                     "remain"};
    };
    const auto unswitched = [] {
      return Failure{1, "tmux_failed",
                     "tmux could not switch the client; the loaded sessions remain"};
    };
    if (caller) {
      const auto server = caller->server();
      if (!server)
        throw unreachable();
      const auto current =
          current_client(*server, caller->active_pane_id(), caller->window_id(), false);
      if (current.connection_identity() != caller->connection_identity() ||
          current.name() != caller->name() || current.pid() != caller->pid() ||
          current.created() != caller->created() || current.tty() != caller->tty())
        throw Failure{1, "tmux_failed",
                      "the invoking client changed; loaded sessions remain"};
      // tmux targets a client name; it cannot atomically check this incarnation.
      const auto switched = current.switch_to(session);
      if (!switched)
        throw unswitched();
    } else if (switch_without_client) {
      // No pane identifies which client to target (a run-shell key binding
      // sets TMUX but not TMUX_PANE): let tmux pick its most recent one.
      const auto server = session.server();
      if (!server)
        throw unreachable();
      const auto switched =
          server->run({"switch-client", "-t", std::string{session.id()}});
      if (!switched)
        throw unswitched();
    } else {
      const auto child = run_child(
          command->argv(),
          {.terminal = true, .terminal_required = true, .timeout = std::nullopt});
      if (child.code != 0)
        throw Failure{child.code, "tmux_failed",
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
  else if (!environment("TMUX").empty())
    command.insert(command.end(), {"-S", tmux_context().socket});
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
    ChildOutput reply{};
    try {
      reply = run_child(command);
    } catch (Failure& spawn) {
      // A child this tool could not start is reported as the script it was;
      // here that child is tmux itself.
      if (spawn.code == "script_failed")
        spawn.code = "tmux_unavailable";
      throw;
    }
    if (reply.code != 0)
      throw Failure{1, "tmux_unavailable", reply.err};
    std::string identity, pid, extra;
    std::istringstream response{reply.out};
    if (!(response >> identity >> pid) || (response >> extra) || identity.size() < 2 ||
        identity.front() != '$' ||
        identity.find_first_not_of("0123456789", 1) != std::string::npos ||
        pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos)
      throw Failure{1, "tmux_failed", "tmux returned an invalid startup identity"};
    const auto server = endpoint(request);
    const auto current_pid = server.run({"display-message", "-p", "#{pid}"});
    if (!current_pid || *current_pid != pid + "\n")
      throw Failure{1, "tmux_failed", "tmux server changed during startup"};
    const auto owned = server.session(identity);
    if (!owned || owned->name() != name)
      throw Failure{1, "tmux_failed", "tmux bootstrap session changed during startup"};
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
Json capture_options(const Server& server, std::string target, bool window = false) {
  CommandRequest command{"show-options"};
  if (window)
    command.push_back("-w");
  command.push_back("-t");
  command.push_back(std::move(target));
  const auto reply = server.run(command);
  if (!reply)
    throw Failure{1, "tmux_failed", reply.error().diagnostic};
  Json options = Json::object();
  for (const auto& entry : parse_options(*reply)) {
    std::string name = entry.name;
    if (entry.index)
      name += "[" + std::to_string(*entry.index) + "]";
    // Without -A, a trailing asterisk is part of a local user option's name.
    if (entry.inherited)
      name += '*';
    options[name] = entry.value;
  }
  return options;
}
// Matches default-shell's basename, or any two ordinary interactive shells
// against each other (macOS names `/bin/sh` bash).
bool names_the_default_shell(std::string_view command, std::string_view login) {
  if (command == login)
    return true;
  static constexpr std::array<std::string_view, 10> ordinary{
      "sh", "bash", "zsh", "dash", "ash", "ksh", "mksh", "fish", "csh", "tcsh"};
  return std::ranges::find(ordinary, command) != ordinary.end() &&
         std::ranges::find(ordinary, login) != ordinary.end();
}
Json capture(const Request& request) {
  const auto server = endpoint(request);
  const auto name = request.value("session");
  const bool by_id = name.size() > 1 && name.front() == '$' &&
                     name.find_first_not_of("0123456789", 1) == std::string::npos;
  const auto session = server.session(by_id ? name : "=" + name + ":");
  // A lookup that finds nothing and a socket with no server behind it are the
  // same answer here, and neither reads as one in the library's own words.
  if (!session)
    throw Failure{1, "session_not_found", "no session named " + name};
  // Never write a document load would refuse. tmux reads "." and ":" in a
  // target as separators, so a session whose name carries one cannot be
  // addressed by name at all -- including by the workspace this would save.
  if (!libtmux::session_target(session->name()))
    throw Failure{1, "invalid_workspace",
                  "session " + std::string{session->name()} +
                      " cannot be captured: tmux reads \".\" and \":\" in a "
                      "session name as target separators, so a workspace naming "
                      "it could not be loaded"};
  const auto windows = session->windows();
  if (!windows)
    throw Failure{1, "tmux_failed", windows.error().diagnostic};
  // A pane sitting at the shell tmux starts for it carries no command of its
  // own, and tmux starts `default-shell`, which is not one of a fixed few.
  const auto shell = session->option("default-shell");
  if (!shell)
    throw Failure{1, "tmux_failed", shell.error().diagnostic};
  const std::string login = fs::path{shell->value}.filename().string();
  Json document{{"session_name", session->name()},
                {"options", capture_options(server, std::string{session->id()})},
                {"windows", Json::array()}};
  for (const auto& window : *windows) {
    const auto panes = window.panes();
    if (!panes)
      throw Failure{1, "tmux_failed", panes.error().diagnostic};
    Json item{{"window_name", window.name()},
              {"window_index", window.index()},
              {"layout", window.layout()},
              {"focus", window.active()},
              {"panes", Json::array()}};
    // load applies options_after once the panes exist, which is what
    // automatic-rename: off needs.
    auto options = capture_options(
        server, std::string{session->id()} + ":" + std::string{window.id()}, true);
    if (!options.empty())
      item["options_after"] = std::move(options);
    for (const auto& pane : *panes) {
      Json pane_item{{"start_directory", pane.path()}, {"focus", pane.active()}};
      // Omit rather than name the shell tmux itself would start for this
      // pane: naming it builds a shell inside a shell on reload.
      const std::string command{pane.command()};
      if (!command.empty() && !names_the_default_shell(command, login))
        pane_item["shell_command"] = Json::array({command});
      item["panes"].push_back(std::move(pane_item));
    }
    document["windows"].push_back(item);
  }
  return document;
}
// A string scalar a YAML 1.1 (PyYAML/tmuxp) or 1.2 resolver would read
// back as bool, null or a number must stay quoted. Rather than reproduce
// both resolvers' grammars (`08`, `0x1F`, `1e3`, `1_000`, `1:30`, ...), quote
// anything that could plausibly start one: empty, a bool/null word in any
// case, or a leading digit/sign/dot/tilde. Over-quoting a string that did not
// need it is harmless; under-quoting corrupts the value.
bool needs_quoting(const std::string& text) {
  if (text.empty())
    return true;
  const auto first = static_cast<unsigned char>(text.front());
  if (std::isdigit(first) != 0 || first == '+' || first == '-' || first == '.' ||
      first == '~')
    return true;
  std::string lower(text.size(), '\0');
  std::ranges::transform(text, lower.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  static const std::set<std::string> words{"y",     "n",  "yes", "no",  "true",
                                           "false", "on", "off", "null"};
  return words.contains(lower);
}
// A YAML::Node's per-scalar style cannot force quoting (YAML::EmitterStyle
// only has Default/Block/Flow); only the streaming Emitter's manipulators
// (YAML::DoubleQuoted) do, and only for the one value that follows them. So
// this drives the Emitter directly rather than building a Node tree.
void emit_yaml(YAML::Emitter& output, const Json& value) {
  if (value.is_null())
    output << YAML::Null;
  else if (value.is_boolean())
    output << value.get<bool>();
  else if (value.is_number_integer() || value.is_number_unsigned())
    output << value.get<long long>();
  else if (value.is_number_float())
    output << value.get<double>();
  else if (value.is_string()) {
    const auto& text = value.get_ref<const std::string&>();
    if (needs_quoting(text))
      output << YAML::DoubleQuoted;
    output << text;
  } else if (value.is_array()) {
    output << YAML::BeginSeq;
    for (const auto& item : value)
      emit_yaml(output, item);
    output << YAML::EndSeq;
  } else {
    output << YAML::BeginMap;
    for (auto entry = value.begin(); entry != value.end(); ++entry) {
      output << YAML::Key << entry.key() << YAML::Value;
      emit_yaml(output, entry.value());
    }
    output << YAML::EndMap;
  }
}
std::string yaml(const Json& document) {
  YAML::Emitter output;
  emit_yaml(output, document);
  if (!output.good())
    throw Failure{1, "invalid_workspace", output.GetLastError()};
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
    throw Failure{2, "usage", "specify --save-to or a machine output mode"};
  const fs::path path = fs::absolute(expand(destination)).lexically_normal();
  if (fs::exists(path) && !request.flag("force"))
    throw Failure{1, "destination_exists",
                  "destination exists; use --force: " + private_path(path)};
  auto temporary_name =
      (path.parent_path() / ("." + path.filename().string() + ".XXXXXX")).string();
  int descriptor = ::mkstemp(temporary_name.data());
  if (descriptor < 0)
    throw Failure{1, "write_failed", "cannot create a temporary output file"};
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
        throw Failure{1, "write_failed", "could not write destination"};
      offset += static_cast<std::size_t>(count);
    }
    const int closed = ::close(descriptor);
    descriptor = -1;
    if (closed != 0)
      throw Failure{1, "write_failed", "could not close destination"};
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
void import_keys(const Json& value, std::initializer_list<std::string_view> allowed,
                 const std::string& where) {
  if (!value.is_object())
    throw Failure{1, "invalid_workspace", where + " must be a mapping"};
  for (auto it = value.begin(); it != value.end(); ++it)
    if (std::ranges::find(allowed, it.key()) == allowed.end())
      throw Failure{1, "unsupported_key", where + "." + it.key() + " is unsupported"};
}

Json import_alias(const Json& value, const char* first, const char* second,
                  const std::string& where) {
  const auto preferred = value.value(first, Json{});
  const auto alternative = value.value(second, Json{});
  if (!preferred.is_null() && !alternative.is_null() && preferred != alternative)
    throw Failure{1, "invalid_workspace",
                  where + "." + first + " conflicts with " + second};
  return preferred.is_null() ? alternative : preferred;
}

Json import_commands(const Json& value, const std::string& where) {
  if (value.is_string() || value.is_null())
    return value;
  if (value.is_array() && std::ranges::all_of(value, [](const Json& command) {
        return command.is_string() || command.is_null();
      }))
    return value;
  throw Failure{1, "invalid_workspace", where + " requires command strings"};
}

std::string import_directory(const Json& value, const fs::path& base,
                             const std::string& where) {
  if (value.is_null())
    return base.string();
  if (!value.is_string())
    throw Failure{1, "invalid_workspace", where + " must be a path string"};
  auto text = value.get<std::string>();
  if (text.find('$') != std::string::npos ||
      (text.starts_with('~') && text != "~" && !text.starts_with("~/")))
    throw Failure{1, "invalid_workspace",
                  where + " cannot preserve its expansion rules in a workspace"};
  fs::path directory{expand(std::move(text))};
  if (directory.is_relative())
    directory = base / directory;
  return directory.lexically_normal().string();
}

Json import_command_group(const Json& value, std::string_view separator,
                          const std::string& where) {
  if (value.is_null() || value.is_string())
    return value;
  if (!value.is_array())
    throw Failure{1, "invalid_workspace",
                  where + " must be a command or command array"};
  std::string result;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (!value[i].is_string())
      throw Failure{1, "invalid_workspace", where + " must contain command strings"};
    if (i != 0)
      result += separator;
    result += value[i].get<std::string>();
  }
  return result;
}

Json imported(Json source, const std::string& kind, const fs::path& path) {
  const bool teamocil = kind == "teamocil";
  if (teamocil && source.contains("session")) {
    import_keys(source, {"session"}, kind);
    source = source["session"];
  }
  if (teamocil)
    import_keys(source,
                {"name", "project_name", "root", "project_root", "windows", "tabs"},
                kind);
  else
    import_keys(source,
                {"name", "project_name", "root", "project_root", "windows", "tabs",
                 "pre_window"},
                kind);
  const auto cwd = fs::current_path();
  const auto root = import_directory(import_alias(source, "project_root", "root", kind),
                                     cwd, kind + ".root");
  Json name = import_alias(source, "project_name", "name", kind);
  if (name.is_null())
    name = path.stem().string();
  if (!name.is_string() || name.get_ref<const std::string&>().empty())
    throw Failure{1, "invalid_workspace", kind + ".name must be a nonempty string"};
  Json result{
      {"session_name", name}, {"start_directory", root}, {"windows", Json::array()}};
  if (source.contains("pre_window"))
    result["shell_command_before"] =
        import_command_group(source["pre_window"], "; ", kind + ".pre_window");
  const auto windows = import_alias(source, "tabs", "windows", kind);
  if (!windows.is_array())
    throw Failure{1, "invalid_workspace", kind + ".windows must be an array"};
  bool window_focused = false;
  for (std::size_t i = 0; i < windows.size(); ++i) {
    const auto where = kind + ".windows[" + std::to_string(i) + "]";
    const auto& raw = windows[i];
    if (!raw.is_object() || (!teamocil && raw.size() != 1))
      throw Failure{1, "invalid_workspace", where + " must describe one window"};
    const auto entry = raw.begin();
    const auto& body = teamocil ? raw : entry.value();
    if (teamocil && (!raw.contains("name") || !raw["name"].is_string() ||
                     raw["name"].get_ref<const std::string&>().empty()))
      throw Failure{1, "invalid_workspace", where + ".name must be a nonempty string"};
    Json window{
        {"window_name", teamocil ? raw.value("name", Json{}) : Json(entry.key())}};
    if (!body.is_object()) {
      window["panes"] =
          Json::array({Json{{"shell_command", import_commands(body, where)}}});
    } else {
      if (teamocil)
        import_keys(body,
                    {"name", "root", "layout", "panes", "splits", "focus", "options"},
                    where);
      else
        import_keys(body, {"root", "layout", "pre", "panes", "synchronize"}, where);
      window["start_directory"] = import_directory(
          body.value("root", Json{}),
          teamocil && body.contains("root") && !body["root"].is_null() ? cwd
                                                                       : fs::path{root},
          where + ".root");
      if (body.contains("layout") && !body["layout"].is_null()) {
        if (!body["layout"].is_string())
          throw Failure{1, "invalid_workspace", where + ".layout must be a string"};
        window["layout"] = body["layout"];
      }
      if (body.contains("focus") && !body["focus"].is_null()) {
        if (!body["focus"].is_boolean())
          throw Failure{1, "invalid_workspace", where + ".focus must be a boolean"};
        const bool focus = body["focus"].get<bool>() && !window_focused;
        window["focus"] = focus;
        window_focused = window_focused || focus;
      }
      if (body.contains("options") && !body["options"].is_null()) {
        if (!body["options"].is_object())
          throw Failure{1, "invalid_workspace", where + ".options must be a mapping"};
        const auto synchronization =
            body["options"].value("synchronize-panes", Json(false));
        if (synchronization != false && synchronization != 0 &&
            synchronization != "off" && synchronization != "false" &&
            synchronization != "no" && synchronization != "0")
          throw Failure{
              1, "invalid_workspace",
              where + ".options.synchronize-panes cannot preserve pre-command timing"};
        window["options"] = body["options"];
      }
      if (body.contains("synchronize") && body["synchronize"] != false &&
          !body["synchronize"].is_null()) {
        if (body["synchronize"] != "after")
          throw Failure{1, "invalid_workspace",
                        where + ".synchronize only supports after-command timing"};
        window["options_after"]["synchronize-panes"] = "on";
      }
      auto panes = import_alias(body, "splits", "panes", where);
      if (panes.is_null())
        panes = Json::array();
      if (!panes.is_array())
        throw Failure{1, "invalid_workspace", where + ".panes must be an array"};
      if (body.contains("pre")) {
        const auto pre = import_command_group(body["pre"], " && ", where + ".pre");
        if (!pre.is_null() && !pre.get_ref<const std::string&>().empty()) {
          if (panes.empty())
            throw Failure{1, "invalid_workspace",
                          where + ".pre requires explicit nonempty panes"};
          window["shell_command_before"] = pre;
        }
      }
      window["panes"] = Json::array();
      bool pane_focused = false;
      for (std::size_t pane = 0; pane < panes.size(); ++pane) {
        const auto pane_where = where + ".panes[" + std::to_string(pane) + "]";
        if (panes[pane].is_object()) {
          if (!teamocil)
            throw Failure{1, "invalid_workspace",
                          pane_where + " uses an unsupported pane title"};
          import_keys(panes[pane], {"commands", "cmd", "focus"}, pane_where);
          Json translated{{"shell_command",
                           import_command_group(
                               import_alias(panes[pane], "commands", "cmd", pane_where),
                               "; ", pane_where + ".commands")}};
          if (panes[pane].contains("focus") && !panes[pane]["focus"].is_null()) {
            if (!panes[pane]["focus"].is_boolean())
              throw Failure{1, "invalid_workspace",
                            pane_where + ".focus must be a boolean"};
            const bool focus = panes[pane]["focus"].get<bool>() && !pane_focused;
            translated["focus"] = focus;
            pane_focused = pane_focused || focus;
          }
          window["panes"].push_back(std::move(translated));
        } else
          window["panes"].push_back(
              Json{{"shell_command", import_commands(panes[pane], pane_where)}});
      }
    }
    result["windows"].push_back(std::move(window));
  }
  if (const auto parsed = workspace::parse_tmuxp(result.dump()); !parsed)
    throw Failure{1, parse_error_code(parsed.error()),
                  kind + ": " + parse_error_message(parsed.error())};
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
    throw Failure{2, "usage", "unknown search field: " + value};
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
      throw Failure{2, "usage", error.what()};
    }
  }
  return result;
}
Json search(const Request& request) {
  const auto compiled = patterns(request);
  Json results = Json::array();
  for (const auto& file : discover()) {
    auto found = record(file, true);
    if (!found)
      continue;
    Json& info = *found;
    std::map<std::string, std::vector<std::string>> fields;
    for (const auto* key : {"name", "session_name", "path"})
      if (info[key].is_string())
        fields[key].push_back(info[key].get<std::string>());
    const auto& config = info["config"];
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
    throw Failure{2, "usage", "88-colour mode is unsupported; use -2 for 256 colours"};
  if (request.command == "search") {
    if (request.list("query").empty())
      throw Failure{2, "usage", "search requires at least one pattern"};
    (void)patterns(request);
  }
  if (request.command == "load" && !request.flag("d")) {
    if (!request.flag("append")) {
      if (request.machine())
        throw Failure{2, "usage", "machine output requires load -d or --append"};
      // Inside tmux an attached load ends in switch-client, which needs no
      // terminal; only outside tmux does it still attach a real client.
      if (environment("TMUX").empty()) {
        if (!request.terminal_allowed)
          throw Failure{2, "usage",
                        "load requires a foreground controlling terminal; use -d"};
        require_terminal();
      }
    }
    if (request.flag("append")) {
      (void)tmux_context();
      const auto pane = environment("TMUX_PANE");
      if (pane.size() < 2 || pane.front() != '%' ||
          pane.find_first_not_of("0123456789", 1) != std::string::npos)
        throw Failure{2, "usage", "TMUX_PANE is not a pane id: " + pane};
    }
  }
  if (request.command == "freeze" && request.value("session").empty())
    throw Failure{2, "usage", "freeze requires a session name or ID"};
  if (request.command == "shell" && !request.flag("c") &&
      (request.machine() || !request.terminal_allowed))
    throw Failure{2, "usage",
                  "interactive shell requires a foreground terminal; use -c"};
}
static Execution execute_impl(const Request& request, const EventSink& event,
                              const PromptSink& prompt) {
  if (request.command == "shell") {
    // TMUX_WORKSPACE_PYTHON selects the interpreter and wins over
    // TMUX_WORKSPACE_TMUXP's executable path; tmuxp has no `__main__`, so
    // call its CLI entry point directly rather than `-m tmuxp`.
    std::vector<std::string> runtime;
    const auto interpreter = environment("TMUX_WORKSPACE_PYTHON");
    if (!interpreter.empty()) {
      runtime = {interpreter, "-u", "-c",
                 "from tmuxp.cli import cli; import sys; cli(sys.argv[1:])"};
    } else {
      auto executable = environment("TMUX_WORKSPACE_TMUXP");
      if (executable.empty())
        executable = "tmuxp";
      runtime = {executable};
    }
    ChildOutput version;
    try {
      auto probe = runtime;
      probe.insert(probe.end(), {"--color", "never", "--version"});
      version = run_child(probe);
    } catch (const Failure&) {
      throw Failure{1, "compatibility_runtime",
                    "shell requires tmuxp 1.74.0; install it, set "
                    "TMUX_WORKSPACE_PYTHON to an interpreter with it installed, "
                    "or set TMUX_WORKSPACE_TMUXP to its executable"};
    }
    if (version.code >= 128)
      throw Failure{version.code, "interrupted", "shell version check interrupted"};
    if (version.code != 0 || !version.out.starts_with("tmuxp 1.74.0, libtmux "))
      throw Failure{1, "compatibility_runtime", "shell requires tmuxp 1.74.0"};
    std::vector<std::string> arguments = runtime;
    arguments.insert(
        arguments.end(),
        {"--color", request.machine() ? "never" : request.value("color", "auto")});
    if (request.flag("log-level")) {
      arguments.push_back("--log-level");
      arguments.push_back(request.value("log-level"));
    }
    arguments.push_back("shell");
    if (request.flag("S")) {
      arguments.push_back("-S");
      arguments.push_back(request.value("S"));
    } else if (request.flag("L")) {
      arguments.push_back("-L");
      arguments.push_back(request.value("L"));
    }
    for (const auto* name :
         {"best", "pdb", "code", "ptipython", "ptpython", "ipython", "bpython"})
      if (request.flag(name))
        arguments.push_back("--" + std::string{name});
    arguments.insert(arguments.end(), request.shell_flags.begin(),
                     request.shell_flags.end());
    if (request.flag("c")) {
      arguments.push_back("-c=" + request.value("c"));
    }
    if (request.flag("session")) {
      arguments.push_back("--");
      arguments.push_back(request.value("session"));
      if (request.flag("window"))
        arguments.push_back(request.value("window"));
    }
    event("started", {{"runtime", "tmuxp 1.74.0"}});
    Json result{{"schema_version", 1},
                {"command", "shell"},
                {"runtime", "tmuxp 1.74.0"},
                {"errors", Json::array()}};
    try {
      const auto child = run_child(
          arguments, {.terminal = !request.flag("c"),
                      .terminal_required = !request.flag("c"),
                      .timeout = std::nullopt,
                      .output = [&](std::string_view stream, std::string_view data) {
                        event("script-output", {{"stream", stream}, {"text", data}});
                      }});
      result["exit_code"] = child.code;
      result["script_output"] = child.value();
    } catch (const Failure& error) {
      result["exit_code"] = error.exit_code;
      result["script_output"] = error.child_output;
      result["errors"].push_back({{"code", error.code}, {"message", error.what()}});
    }
    result["status"] = result.at("exit_code") == 0 ? "ok" : "error";
    // child_status/stdout/stderr/encoding/truncated at the top level, not
    // only nested under script_output: dotnet, go, java, rs and ts all
    // report these names directly, so a consumer written against those five
    // also works here. script_output is kept for callers that already read
    // it; error_handler_t::replace (encoded()'s own dump policy) is what
    // "utf-8-with-replacement" describes -- invalid bytes in the captured
    // text become U+FFFD, they are not rejected.
    result["child_status"] = result.at("script_output").at("exit_code");
    result["stdout"] = result.at("script_output").at("stdout");
    result["stderr"] = result.at("script_output").at("stderr");
    result["truncated"] = result.at("script_output").at("truncated");
    result["encoding"] = "utf-8-with-replacement";
    return {.value = std::move(result)};
  }
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
      if (auto row = record(file, request.flag("full")))
        workspaces.push_back(std::move(*row));
    for (const auto& directory : global_directories())
      dirs.push_back(
          {{"path", private_path(directory)}, {"exists", fs::is_directory(directory)}});
    return {.value = {{"schema_version", 1},
                      {"command", "ls"},
                      {"status", "ok"},
                      {"workspaces", workspaces},
                      {"global_workspace_dirs", dirs}}};
  }
  if (request.command == "search")
    return {.value = {{"schema_version", 1},
                      {"command", "search"},
                      {"status", "ok"},
                      {"results", search(request)}}};
  if (request.command == "debug-info") {
    return {.value = {{"schema_version", 1},
                      {"command", "debug-info"},
                      {"status", "ok"},
                      {"port", "cxx"},
                      {"version", LIBTMUX_WORKSPACE_VERSION},
                      {"compiler", __VERSION__},
                      {"cwd", private_path(fs::current_path())},
                      {"regex", "ECMAScript"}}};
  }
  if (request.command == "convert" || request.command == "import") {
    const auto path = resolve(request.value("workspace-file"), request.importer);
    auto document = read_document(path, request.importer == "tmuxinator");
    if (request.command == "import")
      document = imported(document, request.importer, path);
    const auto format = request.value("workspace-format",
                                      path.extension() == ".json" ? "yaml" : "json");
    if (!request.machine() && !request.flag("yes") && !request.flag("save-to")) {
      return {
          .value = {{"preview", true}, {"format", format}, {"workspace", document}}};
    }
    auto destination = path;
    destination.replace_extension(format == "json" ? ".json" : ".yaml");
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
          throw Failure{1, "invalid_workspace",
                        "before_script must be a command string"};
        const auto command = document["before_script"].get<std::string>();
        if (!command.empty()) {
          try {
            script = split_command(command);
          } catch (const Failure& error) {
            throw Failure{1, "invalid_workspace",
                          "before_script: " + std::string{error.what()}};
          }
          for (const auto& argument : script)
            if (argument.find('\0') != std::string::npos)
              throw Failure{1, "invalid_workspace",
                            "before_script arguments cannot contain NUL"};
        }
      }
      document.erase("before_script");
      if (index + 1 == inputs.size() && !request.value("s").empty())
        document["session_name"] = request.value("s");
      const auto workspace = parse_tmuxp(document.dump());
      if (!workspace)
        throw Failure{1, parse_error_code(workspace.error()),
                      parse_error_message(workspace.error())};
      if (!libtmux::session_target(workspace->session_name))
        throw Failure{1, "invalid_workspace", "session name cannot address itself"};
      std::string directory;
      if (!script.empty()) {
        directory = workspace->start_directory.empty() ? fs::current_path().string()
                                                       : workspace->start_directory;
        if (directory.find('\0') != std::string::npos || !fs::is_directory(directory))
          throw Failure{1, "invalid_workspace",
                        "before_script working directory does not exist"};
      }
      plans.push_back({path, *workspace, std::move(script), std::move(directory)});
    }
    // Said before anything is built: each is something the documents ask for
    // that this build carries on without.
    for (std::size_t index = 0; index < plans.size(); ++index) {
      for (const auto& text : plans[index].workspace.warnings)
        event("warning", {{"input_index", index},
                          {"input", private_path(plans[index].path)},
                          {"code", "unsupported_key"},
                          {"message", text}});
      for (const auto& [where, path] : absent_directories(plans[index].workspace))
        event("warning",
              {{"input_index", index},
               {"input", private_path(plans[index].path)},
               {"code", "invalid_workspace"},
               {"message", where +
                               " is not a directory, so tmux starts the pane in "
                               "$HOME instead: " +
                               path}});
    }
    Json results = Json::array(), errors = Json::array();
    event("started", {{"inputs", plans.size()}});
    Bootstrap bootstrap;
    bool appending = request.flag("append") && !request.flag("d");
    bool retained_changes{};
    int failure_status{1};
    // Distinct from results.size(): a failed input now also gets a results[]
    // record, so "did anything succeed" needs its own count for the
    // ok/error/partial decision below.
    std::size_t succeeded{};
    bool interactive = !request.flag("d") && !appending;
    bool switch_without_client{};
    std::optional<Session> selected;
    std::optional<Client> caller;
    std::string stage{"startup"};
    std::size_t active_input{};
    try {
      auto server = endpoint(request);
      for (const auto& plan : plans)
        if (auto error = validate_layouts(server, plan.workspace))
          throw Failure{1, "invalid_workspace",
                        "windows[" + std::to_string(error->window_index) +
                            "].layout: " + error->reason};
      // Interactive, unforced: ask before moving the user, never in machine
      // mode and never without a terminal to answer from.
      if (interactive && !request.flag("yes") && !request.machine() && !plans.empty()) {
        const auto& target_name = plans.back().workspace.session_name;
        if (server.session("=" + target_name + ":")) {
          const auto typed =
              prompt ? prompt(target_name + " is already running. Attach? [Y/n]") : "";
          const char answer =
              typed.empty() ? 'y' : static_cast<char>(std::tolower(typed.front()));
          if (answer != 'y')
            return {.value = {{"schema_version", 1},
                              {"command", "load"},
                              {"status", "ok"},
                              {"results", Json::array()},
                              {"errors", Json::array()}},
                    .exit_code = 0};
        } else if (!environment("TMUX").empty()) {
          const auto typed =
              prompt ? prompt("Already inside tmux: switch (y), load detached (n), or "
                              "append (a)? [y/n/a]")
                     : "";
          const char answer =
              typed.empty() ? 'y' : static_cast<char>(std::tolower(typed.front()));
          if (answer == 'n')
            interactive = false;
          else if (answer == 'a') {
            appending = true;
            interactive = false;
          }
        }
      }
      std::optional<Session> borrowed;
      if (appending)
        borrowed = append_target(server);
      else if (interactive && !environment("TMUX").empty()) {
        if (!environment("TMUX_PANE").empty()) {
          const auto pane = current_pane(server);
          caller = current_client(server, pane.id(), pane.window_id());
        } else {
          // No controlling pane to name a client from (a run-shell key
          // binding sets TMUX but not TMUX_PANE): confirm this is still the
          // server TMUX names, then let tmux pick its most recent client.
          if (!same_socket(tmux_context().socket, server.socket_path()))
            throw Failure{
                2, "usage",
                "selected server differs from the current pane's server; use -d"};
          switch_without_client = true;
        }
      } else if (!server.is_alive())
        server = start_endpoint(request, bootstrap);
      stage = "load";
      const auto [session_width, session_height] =
          session_dimensions(request.stdout_terminal);
      for (std::size_t index = 0; index < plans.size(); ++index) {
        active_input = index;
        const auto& plan = plans[index];
        check_interruption();
        std::size_t total_panes{};
        for (const auto& window : plan.workspace.windows)
          total_panes += window.panes.size();
        event("workspace-started", {{"input_index", index},
                                    {"input", private_path(plan.path)},
                                    {"session_name", plan.workspace.session_name},
                                    {"window_total", plan.workspace.windows.size()},
                                    {"session_pane_total", total_panes}});
        std::optional<Failure> observer_error;
        // Captured so a failed build can still report the session it
        // attempted, even though the session itself may since have
        // been rolled back.
        std::string attempted_session_id;
        const BuildObserver observer =
            [&](const BuildEvent& update) -> std::optional<std::string> {
          try {
            check_interruption();
            if (update.phase == BuildPhase::waiting)
              return std::nullopt;
            if (update.phase == BuildPhase::session_started) {
              // Reported the moment the session exists, directly after
              // workspace-started and before any window; skipped for a
              // borrowed (appended) session, which was not created here.
              attempted_session_id = update.session_id;
              if (!borrowed)
                event("session-created", {{"input_index", index},
                                          {"input", private_path(plan.path)},
                                          {"session_id", update.session_id},
                                          {"session_name", plan.workspace.session_name},
                                          {"action", "created"},
                                          {"reused", false}});
              return std::nullopt;
            }
            const bool is_window = update.phase == BuildPhase::window_started ||
                                   update.phase == BuildPhase::window_completed;
            const auto name =
                update.phase == BuildPhase::window_started     ? "window-created"
                : update.phase == BuildPhase::window_completed ? "window-completed"
                : update.phase == BuildPhase::pane_started     ? "pane-created"
                                                               : "pane-completed";
            const auto& window = plan.workspace.windows.at(update.window_index);
            Json fields{{"input_index", index},
                        {"session_id", update.session_id},
                        {"window_id", update.window_id},
                        {"window_index", update.window_index + 1},
                        {"window_name", window.name}};
            if (!is_window) {
              fields["pane_id"] = update.pane_id;
              fields["pane_index"] = update.pane_index + 1;
              fields["pane_total"] = window.panes.size();
            }
            event(name, std::move(fields));
            return std::nullopt;
          } catch (const Failure& error) {
            observer_error = error;
          } catch (const std::exception& error) {
            observer_error.emplace(1, "tmux_failed", error.what());
          }
          return observer_error->what();
        };
        const auto existing =
            borrowed ? libtmux::expected<Session, CommandFailure>{*borrowed}
                     : server.session("=" + plan.workspace.session_name + ":");
        // Reusing a session compares it against the document; converging on
        // the document is a separate operation this one does not perform. A
        // session that does not satisfy the document stops the load, naming
        // what it lacks, and is left exactly as it was found -- nothing here
        // is built and nothing is changed.
        if (!borrowed && existing) {
          const auto absent = absent_windows(*existing, plan.workspace);
          if (!absent || !absent->empty()) {
            failure_status = 1;
            Json problem{
                {"code", absent ? "session_mismatch" : "tmux_failed"},
                {"message",
                 absent ? "session " + plan.workspace.session_name +
                              " is already running and does not have the window " +
                              absent->front() + " this workspace describes"
                        : "the windows of the session already running as " +
                              plan.workspace.session_name + " could not be read"},
                {"input_index", index},
                {"failed_stage", stage},
                {"session_id", existing->id()},
                {"session_name", existing->name()}};
            if (absent)
              problem["missing_windows"] = *absent;
            errors.push_back(std::move(problem));
            results.push_back({{"input", private_path(plan.path)},
                               {"input_index", index},
                               {"session_id", existing->id()},
                               {"session_name", plan.workspace.session_name},
                               {"reused", true},
                               {"action", "reused"}});
            break;
          }
        }
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
              if (child.code != 0)
                script_error.emplace(
                    child.code >= 128 ? child.code : 1, "script_failed",
                    "before_script exited with status " + std::to_string(child.code));
              // child_status/truncated match go's script-completed fields, so
              // a consumer reads the child's status without digging into the
              // nested script_output object; script_output is kept for the
              // full capture (stdout/stderr/encoding).
              event("script-completed", {{"input_index", index},
                                         {"child_status", child.code},
                                         {"truncated", child.truncated},
                                         {"script_output", script_output}});
              if (script_error)
                return script_error->what();
            } catch (const Failure& error) {
              if (!script_error)
                script_error = error;
              if (script_output.is_null())
                script_output = error.child_output;
              return script_error->what();
            }
            stage = "load";
            return std::nullopt;
          };
        }
        auto built = borrowed   ? append(*borrowed, plan.workspace, before, observer)
                     : existing ? libtmux::expected<Session, BuildError>{*existing}
                                : build(server, plan.workspace, before, observer,
                                        session_width, session_height);
        if (!built) {
          if (!script_error && observer_error)
            script_error = observer_error;
          // A group-wide signal can kill the tmux child a step was waiting on
          // before check_interruption() sees the flag; reclassify that here
          // as the cancellation it was.
          if (!script_error) {
            try {
              check_interruption();
            } catch (Failure& cancelled) {
              script_error = std::move(cancelled);
            }
          }
          if (script_error)
            failure_status = script_error->exit_code;
          Json problem{
              {"code", script_error ? script_error->code : "tmux_failed"},
              {"message", script_error ? script_error->what() : built.error().reason},
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
          // One record per attempted input, failed inputs included: its
          // session may already be gone (an owned build rolls back on
          // failure), but the id an in-progress build reported and this
          // input's own document are still known. Json(string) rather than
          // Json{string}: the brace form is nlohmann's initializer-list
          // constructor, which turns a single string into a one-element
          // array instead of a string value.
          Json failed_result{
              {"input", private_path(plan.path)},
              {"input_index", index},
              {"session_id",
               attempted_session_id.empty() ? Json{} : Json(attempted_session_id)},
              {"session_name", plan.workspace.session_name},
              {"reused", static_cast<bool>(borrowed) || static_cast<bool>(existing)},
              {"action", borrowed   ? "appended"
                         : existing ? "reused"
                                    : "created"}};
          results.push_back(std::move(failed_result));
          break;
        }
        Json result{
            {"input", private_path(plan.path)},
            {"input_index", index},
            {"session_id", built->id()},
            {"session_name", built->name()},
            {"reused", static_cast<bool>(borrowed) || static_cast<bool>(existing)},
            {"action", borrowed   ? "appended"
                       : existing ? "reused"
                                  : "created"}};
        if (!script_output.is_null())
          result["script_output"] = script_output;
        results.push_back(result);
        ++succeeded;
        if (index + 1 == plans.size())
          selected = *built;
        // session-created already fired from the build observer, directly
        // after workspace-started, for a session created here.
        event("workspace-completed", result);
      }
    } catch (const Failure& error) {
      failure_status = error.exit_code;
      Json problem{{"code", error.code},
                   {"message", error.what()},
                   {"input_index", active_input},
                   {"failed_stage", stage}};
      if (!error.retained_state.is_null())
        problem["retained_state"] = error.retained_state;
      errors.push_back(std::move(problem));
    } catch (const std::exception& error) {
      errors.push_back({{"code", "tmux_failed"},
                        {"message", error.what()},
                        {"input_index", active_input},
                        {"failed_stage", stage}});
    }
    if (bootstrap.session) {
      const auto cleaned = bootstrap.session->kill();
      bootstrap.session.reset();
      if (!cleaned)
        errors.push_back({{"code", "tmux_failed"},
                          {"message", cleaned.error().diagnostic},
                          {"failed_stage", "cleanup"}});
    }
    // A failed input still gets a results[] record, so whether anything
    // succeeded needs its own counter rather than results.empty().
    const auto status = errors.empty()                        ? "ok"
                        : succeeded == 0 && !retained_changes ? "error"
                                                              : "partial";
    // The envelope carries only schema_version, command, status, results and
    // errors; the process exit code travels out of band on Execution, not in
    // the document a --json/--ndjson consumer reads.
    Json summary{{"schema_version", 1},
                 {"command", "load"},
                 {"status", status},
                 {"results", results},
                 {"errors", errors}};
    Execution execution{.value = std::move(summary),
                        .exit_code = errors.empty() ? 0 : failure_status};
    if (interactive && errors.empty() && selected) {
      try {
        execution.handoff =
            load_handoff(*selected, std::move(caller), switch_without_client);
      } catch (Failure& error) {
        error.retained_state = std::move(execution.value);
        throw;
      }
    }
    return execution;
  }
  throw Failure{2, "usage", "command is not implemented"};
}
Execution execute(const Request& request, const EventSink& event,
                  const PromptSink& prompt) {
  if (request.command == "load" || request.command == "shell")
    return with_interrupts([&] { return execute_impl(request, event, prompt); });
  return execute_impl(request, event, prompt);
}

std::string human_result(const Request& request, const Json& result, bool colour) {
  const auto role = [colour](const std::string& code, const std::string& value) {
    const auto text = human_text(value);
    return colour ? "\033[" + code + "m" + text + "\033[0m" : text;
  };
  std::ostringstream output;
  if (result.is_object() && result.value("preview", false)) {
    return result.at("format") == "json" ? encoded(result.at("workspace"), 2) + "\n"
                                         : yaml(result.at("workspace"));
  }
  if (request.command == "edit")
    return result.at("stdout").get<std::string>();
  if (request.command == "shell")
    return {};
  if (request.command == "ls" || request.command == "search") {
    const auto& rows = result.at(request.command == "ls" ? "workspaces" : "results");
    const auto write = [&](const Json& row, std::string_view branch,
                           std::string_view continuation) {
      output << branch << role("1;35", row.at("name").get<std::string>()) << "  "
             << role("36", row.at("path").get<std::string>()) << '\n';
      if (request.command == "ls" && request.flag("full")) {
        std::istringstream document{visible_controls(encoded(row.at("config"), 2))};
        for (std::string line; std::getline(document, line);)
          output << continuation << "  " << line << '\n';
      }
    };
    if (request.command == "ls" && request.flag("tree")) {
      struct Group {
        std::string directory;
        std::vector<const Json*> rows;
      };
      std::vector<Group> groups;
      for (const auto& row : rows) {
        const auto directory =
            fs::path{row.at("path").get<std::string>()}.parent_path().string();
        auto group = std::ranges::find(groups, directory, &Group::directory);
        if (group == groups.end()) {
          groups.push_back({directory, {&row}});
        } else
          group->rows.push_back(&row);
      }
      for (const auto& group : groups) {
        output << role("36", group.directory) << '\n';
        for (std::size_t index = 0; index < group.rows.size(); ++index) {
          const bool last = index + 1 == group.rows.size();
          write(*group.rows[index], last ? "└── " : "├── ", last ? "    " : "│   ");
        }
      }
    } else {
      for (const auto& row : rows)
        write(row, {}, {});
    }
  } else if (request.command == "load") {
    // results[] now carries one record per attempted input, failed ones
    // included; the human summary line is only for what actually built, so
    // skip any input_index that also has an errors[] entry.
    std::set<std::size_t> failed_inputs;
    for (const auto& problem : result.at("errors"))
      if (problem.contains("input_index"))
        failed_inputs.insert(problem.at("input_index").get<std::size_t>());
    for (const auto& item : result.at("results")) {
      if (failed_inputs.contains(item.at("input_index").get<std::size_t>()))
        continue;
      // Appending names the session that received the windows, not a session
      // this input created or reused, so it carries no session_id.
      if (item.at("action") == "appended") {
        output << role("32", "Appended") << ' '
               << role("1;35", item.at("session_name").get<std::string>()) << '\n';
        continue;
      }
      output << role("32", item.at("action").get<std::string>()) << ' '
             << role("1;35", item.at("session_name").get<std::string>()) << ' '
             << role("2", item.at("session_id").get<std::string>()) << '\n';
    }
  } else if (result.contains("destination")) {
    if (!request.flag("quiet"))
      output << role("32", "Saved") << ' '
             << role("36", result.at("destination").get<std::string>()) << '\n';
  } else if (request.command == "debug-info") {
    // Human mode never emits JSON; `--json` is what keeps the object, and
    // the envelope it shares with every other command is for that reader.
    for (const auto& [key, value] : result.items()) {
      if (key == "schema_version" || key == "command" || key == "status")
        continue;
      output << role("1;35", key) << ": "
             << role("36", value.is_string() ? value.get<std::string>() : value.dump())
             << '\n';
    }
  } else
    output << encoded(result, 2) << '\n';
  return output.str();
}
} // namespace libtmux::workspace::cli
