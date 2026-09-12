#include "progress.hpp"
#include "services.hpp"
#include "workspace_cli.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string_view>

#include <CLI/CLI.hpp>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace libtmux::workspace::cli {
namespace {
class DiagnosticLog {
  const Request& request_;
  std::ostream& errors_;
  int descriptor_{-1}, level_{2}, failure_{};
  bool reported_{};

  static int rank(std::string_view level) {
    if (level == "debug")
      return 0;
    if (level == "info")
      return 1;
    if (level == "error")
      return 3;
    return level == "critical" ? 4 : 2;
  }
  static void omit_capture(Json& data) {
    if (data.is_object())
      data.erase("script_output");
    if (data.is_structured())
      for (auto& child : data)
        omit_capture(child);
  }
  void close() noexcept {
#ifndef _WIN32
    if (descriptor_ >= 0 && ::close(descriptor_) != 0 && failure_ == 0)
      failure_ = errno;
#endif
    descriptor_ = -1;
  }
  void write(const Json& record) {
#ifndef _WIN32
    const auto bytes = encoded(record) + '\n';
    std::size_t offset{};
    while (offset < bytes.size()) {
      const auto count =
          ::write(descriptor_, bytes.data() + offset, bytes.size() - offset);
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0) {
        failure_ = count == 0 ? EIO : errno;
        close();
        return;
      }
      offset += static_cast<std::size_t>(count);
    }
#else
    (void)record;
#endif
  }

public:
  DiagnosticLog(const Request& request, std::ostream& errors)
      : request_{request}, errors_{errors} {}
  DiagnosticLog(const DiagnosticLog&) = delete;
  DiagnosticLog& operator=(const DiagnosticLog&) = delete;
  ~DiagnosticLog() {
    close();
    report_failure();
  }
  bool enabled(std::string_view severity) const { return rank(severity) >= level_; }
  void configure() {
    level_ = rank(request_.value("log-level", "warning"));
    if (!request_.flag("log-file"))
      return;
#ifndef _WIN32
    const auto path = request_.value("log-file");
    if (path.find('\0') != std::string::npos)
      throw Failure{1, "LOG_FILE_UNAVAILABLE", "log path contains a null byte"};
    struct stat status {};
    const int inspected = ::lstat(path.c_str(), &status);
    if (inspected == 0 && !S_ISREG(status.st_mode))
      throw Failure{1, "LOG_FILE_UNAVAILABLE",
                    "log destination must be a regular file"};
    if (inspected != 0 && errno != ENOENT)
      throw Failure{1, "LOG_FILE_UNAVAILABLE", std::strerror(errno)};
    const int file = ::open(path.c_str(),
                            O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_NONBLOCK |
                                O_CLOEXEC | O_NOCTTY,
                            0600);
    if (file < 0)
      throw Failure{1, "LOG_FILE_UNAVAILABLE", std::strerror(errno)};
    const int verified = ::fstat(file, &status);
    const int cause = errno;
    if (verified != 0 || !S_ISREG(status.st_mode)) {
      (void)::close(file);
      throw Failure{1, "LOG_FILE_UNAVAILABLE",
                    verified != 0 ? std::strerror(cause)
                                  : "opened log destination is not a regular file"};
    }
    descriptor_ = file;
#else
    throw Failure{1, "LOG_FILE_UNAVAILABLE",
                  "log files require POSIX file descriptors"};
#endif
  }
  void event(const std::string& name, const Json& data, std::size_t sequence) noexcept {
    const std::string_view severity = name == "script-output" ? "debug"
                                      : name == "failed"      ? "error"
                                                              : "info";
    if (descriptor_ < 0 || !enabled(severity))
      return;
    try {
      auto fields = data;
      if (name == "script-completed" && fields.contains("script_output")) {
        fields["exit_code"] = fields.at("script_output").at("exit_code");
        fields["truncated"] = fields.at("script_output").at("truncated");
      }
      if (name != "script-output")
        omit_capture(fields);
      write({{"schema_version", 1},
             {"command", request_.command},
             {"event", name},
             {"sequence", sequence},
             {"severity", severity},
             {"data", fields}});
    } catch (...) {
      failure_ = EIO;
      close();
    }
  }
  void diagnostic(const std::string& code, const std::string& message) noexcept {
    if (descriptor_ < 0 || !enabled("error"))
      return;
    try {
      write({{"schema_version", 1},
             {"command", request_.command},
             {"severity", "error"},
             {"code", code},
             {"message", message}});
    } catch (...) {
      failure_ = EIO;
      close();
    }
  }
  void report_failure() noexcept {
    if (failure_ == 0 || reported_ || !enabled("warning"))
      return;
    reported_ = true;
    try {
      const auto message = std::string{"log file disabled: "} + std::strerror(failure_);
      if (request_.machine())
        errors_ << encoded({{"code", "LOG_FILE_WRITE_FAILED"},
                            {"level", "warning"},
                            {"message", message}})
                << '\n';
      else
        errors_ << "Warning: " << message << '\n';
      errors_.flush();
    } catch (...) {
      // An optional diagnostic cannot replace the operation's status.
    }
  }
};

class Model {
public:
  CLI::App root{"Manage tmux workspaces from YAML or JSON", "tmux-workspace"};
  Model() {
    root.require_subcommand(0, 1);
    root.set_version_flag("-V,--version", LIBTMUX_WORKSPACE_VERSION);
    root.add_option("--color", "Colour policy: auto, always or never")
        ->check(CLI::IsMember({"auto", "always", "never"}))
        ->default_str("auto");
    root.add_option(
            "--log-level",
            "Optional diagnostic level (default warning); errors remain visible")
        ->check(CLI::IsMember({"debug", "info", "warning", "error", "critical"}))
        ->default_str("warning");
    root.add_flag("--command-tree", "Print command metadata as JSON");
    machine(root);
    auto* load = root.add_subcommand("load", "Create or reuse configured sessions");
    load->add_option("workspace-file", "Workspace paths or names")
        ->expected(1, -1)
        ->required();
    sockets(*load);
    load->add_option("-f", "tmux configuration file");
    load->add_option("-s", "Override the session name");
    load->add_flag("-y,--yes", "Answer yes to confirmation prompts");
    load->add_flag("-d", "Load without attaching a terminal");
    load->add_flag("-a,--append", "Append windows to the current session");
    auto* two = load->add_flag("-2", "Use 256-colour tmux mode");
    load->add_flag("-8", "Unsupported legacy 88-colour mode; use -2")->excludes(two);
    load->add_option(
        "--log-file",
        "Append JSON diagnostics; info logs lifecycle, debug adds script output");
    load->add_option("--progress-format", "Progress preset or template")
        ->envname("TMUXP_PROGRESS_FORMAT");
    load->add_option("--progress-lines", "Script panel lines; -1 uses terminal height")
        ->check(CLI::Validator{
            [](std::string& value) {
              const auto error = CLI::Range(-1, std::numeric_limits<int>::max())(value);
              if (!error.empty())
                // CLI11 discards invalid environment defaults after ValidationError.
                throw CLI::ConversionError{"--progress-lines: " + error};
              return std::string{};
            },
            "INT>=-1"})
        ->envname("TMUXP_PROGRESS_LINES")
        ->default_str("3");
    load->add_flag("--no-progress", "Disable terminal progress");
    auto* freeze = root.add_subcommand("freeze", "Capture a running session");
    freeze->add_option("session", "Session name or ID");
    sockets(*freeze);
    freeze->add_option("-f,--workspace-format", "Saved workspace encoding")
        ->check(CLI::IsMember({"yaml", "json"}));
    freeze->add_option("-o,--save-to", "Output path; machine mode defaults to stdout");
    freeze->add_flag("-y,--yes", "Answer yes to confirmation prompts");
    freeze->add_flag("-q,--quiet", "Suppress human status text");
    freeze->add_flag("--force", "Replace an existing destination");
    auto* convert = root.add_subcommand("convert", "Convert YAML and JSON workspaces");
    convert->add_option("workspace-file", "Workspace path or name")->required();
    convert->add_flag("-y,--yes", "Answer yes to confirmation prompts");
    save_options(*convert);
    auto* importer = root.add_subcommand("import", "Import another workspace format");
    importer->require_subcommand(1);
    for (const auto* name : {"teamocil", "tmuxinator"}) {
      auto* child = importer->add_subcommand(name, "Import a " + std::string{name} +
                                                       " workspace");
      child->add_option("workspace-file", "Source path or workspace name")->required();
      save_options(*child);
      machine(*child);
    }
    auto* list = root.add_subcommand("ls", "List discovered workspaces");
    list->add_flag("--tree", "Group workspaces by directory");
    list->add_flag("--full", "Include parsed workspace content");
    auto* edit = root.add_subcommand("edit", "Open a workspace in VISUAL or EDITOR");
    edit->add_option("workspace-file", "Workspace path or name")->required();
    root.add_subcommand("debug-info", "Show runtime and tmux diagnostics");
    auto* shell =
        root.add_subcommand("shell", "Open a version-checked Python tmuxp shell");
    shell->add_option("session", "Session name or ID");
    shell->add_option("window", "Window name or index");
    sockets(*shell);
    shell->add_option("-c", "Execute Python and return its output");
    auto* backend = shell->add_option_group("Python backend")->require_option(0, 1);
    for (const auto* name : {"--best", "--pdb", "--code", "--ptipython", "--ptpython",
                             "--ipython", "--bpython"}) {
      backend->add_flag(name, "Select the " + std::string{name + 2} + " backend");
    }
    for (const auto* name :
         {"--use-pythonrc", "--no-startup", "--use-vi-mode", "--no-vi-mode"}) {
      shell->add_flag(name, "Configure Python shell startup");
    }
    auto* search = root.add_subcommand(
        "search", "Search workspace fields with native regular expressions");
    search->add_option("query", "Patterns with optional field prefixes")
        ->expected(0, -1);
    search->add_option("-f,--field", "Search field; repeat to select more")
        ->expected(1)
        ->allow_extra_args(false)
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
    for (const auto& [name, description] :
         {std::pair{"-i,--ignore-case", "Match without case distinctions"},
          {"-S,--smart-case", "Ignore case for lowercase patterns"},
          {"-F,--fixed-strings", "Interpret patterns as literal text"},
          {"-w,--word-regexp", "Match whole words, grouping alternatives"},
          {"-v,--invert-match", "Select workspaces that do not match"},
          {"--any", "Accept any pattern instead of requiring every pattern"}}) {
      search->add_flag(name, description);
    }
    for (auto* child : root.get_subcommands(
             [](const CLI::App* node) { return !node->get_name().empty(); })) {
      machine(*child);
    }
  }
  static void machine(CLI::App& app) {
    app.add_flag("--json", "Write structured JSON")->disable_flag_override();
    app.add_flag("--ndjson", "Write flushed JSON records; takes precedence over --json")
        ->disable_flag_override();
  }
  static void sockets(CLI::App& app) {
    app.add_option("-L", "tmux socket name");
    app.add_option("-S", "tmux socket path; takes precedence over -L");
  }
  static void save_options(CLI::App& app) {
    app.add_option("--save-to", "Output path; machine mode defaults to stdout");
    app.add_option("--workspace-format", "Output encoding")
        ->check(CLI::IsMember({"yaml", "json"}));
    app.add_flag("--force", "Replace an existing destination");
  }
};
void collect(CLI::App& node, Request& request) {
  for (auto* option : node.get_options()) {
    if (option->count() != 0) {
      for (const auto& name : option->get_lnames())
        request.values[name] = option->results();
      for (const auto& name : option->get_snames())
        request.values[name] = option->results();
      if (option->get_positional())
        request.values[option->get_name()] = option->results();
    }
  }
  for (auto* child : node.get_subcommands())
    collect(*child, request);
}
Json metadata(const CLI::App& node) {
  Json options = Json::array();
  for (const auto* option : node.get_options()) {
    Json aliases = Json::array();
    for (const auto& name : option->get_snames())
      aliases.push_back("-" + name);
    for (const auto& name : option->get_lnames())
      aliases.push_back("--" + name);
    options.push_back({{"name", option->get_name()},
                       {"aliases", aliases},
                       {"positional", option->get_positional()},
                       {"description", option->get_description()},
                       {"required", option->get_required()},
                       {"default", option->get_default_str()},
                       {"minimum", option->get_expected_min()},
                       {"maximum", option->get_expected_max()}});
  }
  Json children = Json::array();
  for (const auto* child : node.get_subcommands([](const CLI::App*) { return true; })) {
    children.push_back(metadata(*child));
  }
  return {{"name", node.get_name()},
          {"description", node.get_description()},
          {"options", options},
          {"children", children}};
}
bool colour_enabled(const Request& request, std::ostream& output) {
  const auto environment = [](const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0';
  };
  const auto policy = request.value("color", "auto");
  if (request.machine() || environment("NO_COLOR") || policy == "never")
    return false;
  if (policy == "always" || environment("FORCE_COLOR"))
    return true;
#ifndef _WIN32
  return &output == &std::cout && ::isatty(STDOUT_FILENO) != 0;
#else
  (void)output;
  return false;
#endif
}
} // namespace
int run(std::vector<std::string> arguments, std::istream& input, std::ostream& output,
        std::ostream& errors) {
  Request request;
  // A supplied input stream does not authorize borrowing the process terminal.
  request.terminal_allowed = &input == &std::cin;
  for (const auto& arg : arguments) {
    if (arg == "--")
      break;
    if (arg == "--json")
      request.json = true;
    if (arg == "--ndjson")
      request.ndjson = true;
  }
  Model model;
  DiagnosticLog diagnostics{request, errors};
  Execution execution;
  std::unique_ptr<Progress> progress;
  Json retained_state;
  const auto fail = [&](int status, const std::string& code,
                        const std::string& message) {
    if (progress)
      progress->finish();
    diagnostics.diagnostic(code, message);
    try {
      if (request.command == "load" && execution.value.is_object()) {
        const auto primary = execution.value.at("exit_code").get<int>();
        if (primary != 0)
          status = primary;
      }
      if (retained_state.is_null() && request.command == "load" &&
          execution.value.is_object())
        retained_state = execution.value;
      if (request.machine()) {
        Json diagnostic{{"code", code}, {"message", message}};
        if (!retained_state.is_null())
          diagnostic["retained_state"] = retained_state;
        errors << encoded(diagnostic) << '\n';
      } else {
        errors << "Error: " << message << '\n';
        if (!retained_state.is_null())
          errors << "Retained state: " << encoded(retained_state) << '\n';
      }
    } catch (const std::exception&) {
      // A failed diagnostic sink does not replace the operation's status.
    }
    return status;
  };
  try {
    std::reverse(arguments.begin(), arguments.end());
    model.root.parse(arguments);
    collect(model.root, request);
    if (request.flag("command-tree")) {
      output << encoded(metadata(model.root), 2) << '\n';
      return output ? 0 : 1;
    }
    auto parsed = model.root.get_subcommands();
    if (parsed.empty()) {
      if (request.machine())
        throw Failure{2, "USAGE", "a command is required"};
      output << model.root.help();
      return output ? 0 : 1;
    }
    request.command = parsed.front()->get_name();
    if (request.command == "import")
      request.importer = parsed.front()->get_subcommands().front()->get_name();
    validate(request);
    diagnostics.configure();
    progress = std::make_unique<Progress>(request, output, errors,
                                          progress_terminal(request, output, errors));
    std::size_t sequence{};
    const auto emit = [&](const std::string& name, Json data) {
      diagnostics.event(name, data, ++sequence);
      progress->event(name, data);
      if (!request.ndjson)
        return;
      data["schema_version"] = 1;
      data["command"] = request.command;
      data["event"] = name;
      data["sequence"] = sequence;
      output << encoded(data) << '\n' << std::flush;
      if (!output)
        throw Failure{1, "OUTPUT_CLOSED", "output stream closed"};
    };
    const auto operation = [&] {
      try {
        auto result = execute(request, emit);
        progress->finish(result.value.is_object() && result.value.contains("status") &&
                         result.value.at("status") != "ok");
        return result;
      } catch (...) {
        progress->finish(true);
        throw;
      }
    };
    execution = request.command == "load" ? with_interrupts(operation) : operation();
    const auto& result = execution.value;
    if (request.command == "freeze" && request.json && !request.ndjson &&
        !result.contains("destination") && diagnostics.enabled("warning")) {
      errors << encoded({{"code", "CAPTURE_LOSSY"},
                         {"level", "warning"},
                         {"message", "Capture cannot recover original command "
                                     "arguments, history, plugins or before scripts."}})
             << '\n';
    }
    const bool failed = (request.command == "load" || request.command == "edit") &&
                        result.at("status") != "ok";
    if (request.command == "edit" && !request.machine())
      errors << result.at("stderr").get<std::string>();
    if (failed && request.command == "load") {
      for (const auto& error : result.at("errors")) {
        diagnostics.diagnostic(error.at("code").get<std::string>(),
                               error.at("message").get<std::string>());
        if (request.machine()) {
          auto diagnostic = error;
          diagnostic.erase("script_output");
          errors << encoded(diagnostic) << '\n';
        } else
          errors << "Error: " << error.at("message").get<std::string>() << '\n';
      }
    }
    if (!request.ndjson && (request.command == "load" || request.command == "edit"))
      diagnostics.event(failed ? "failed" : "completed", result, ++sequence);
    if (request.ndjson) {
      if (request.command == "load" || request.command == "edit")
        emit(failed ? "failed" : "completed", result);
      else if (request.command == "ls") {
        for (const auto& item : result.at("workspaces"))
          output << encoded(item) << '\n';
      } else if (request.command == "search") {
        for (const auto& item : result)
          output << encoded(item) << '\n';
      } else
        output << encoded(result) << '\n';
    } else if (request.json)
      output << encoded(result, 2) << '\n';
    else
      output << human_result(request, result, colour_enabled(request, output));
    if (request.command == "load") {
      output.flush();
      errors.flush();
      if (!output || !errors)
        throw Failure{1, "OUTPUT_CLOSED", "load output stream closed"};
      diagnostics.report_failure();
      if (execution.handoff)
        execution.handoff();
    }
    if ((request.command == "edit" || request.command == "load") && output)
      return result.at("exit_code").get<int>();
    return output && !failed ? 0 : 1;
  } catch (const CLI::ParseError& error) {
    if (error.get_exit_code() == 0)
      return model.root.exit(error, output, errors);
    if (request.machine())
      errors << encoded({{"code", "USAGE"}, {"message", error.what()}}) << '\n';
    else
      model.root.exit(error, output, errors);
    return 2;
  } catch (const Failure& error) {
    if (!error.retained_state.is_null())
      retained_state = error.retained_state;
    return fail(error.exit_code, error.code, error.what());
  } catch (const std::exception& error) {
    return fail(1, "OPERATION_FAILED", error.what());
  }
}
} // namespace libtmux::workspace::cli
