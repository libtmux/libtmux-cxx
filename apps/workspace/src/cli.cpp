#include "services.hpp"
#include "workspace_cli.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string_view>

#include <CLI/CLI.hpp>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace libtmux::workspace::cli {
namespace {
class Model {
public:
  CLI::App root{"Manage tmux workspaces from YAML or JSON", "tmux-workspace"};
  Model() {
    root.require_subcommand(0, 1);
    root.set_version_flag("-V,--version", LIBTMUX_WORKSPACE_VERSION);
    root.add_option("--color", "Colour policy: auto, always or never")
        ->check(CLI::IsMember({"auto", "always", "never"}))
        ->default_str("auto");
    root.add_option("--log-level", "Diagnostic level")
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
    load->add_flag("-8", "Use 88-colour tmux mode")->excludes(two);
    load->add_option("--log-file", "Write diagnostic records to a file");
    load->add_option("--progress-format", "Progress preset or template");
    load->add_option("--progress-lines", "Script panel lines; -1 uses terminal height")
        ->check(CLI::TypeValidator<int>())
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
  (void)input;
  Request request;
  for (const auto& arg : arguments) {
    if (arg == "--")
      break;
    if (arg == "--json")
      request.json = true;
    if (arg == "--ndjson")
      request.ndjson = true;
  }
  Model model;
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
    std::size_t sequence{};
    const auto emit = [&](const std::string& name, Json data) {
      if (!request.ndjson)
        return;
      data["schema_version"] = 1;
      data["command"] = request.command;
      data["event"] = name;
      data["sequence"] = ++sequence;
      output << encoded(data) << '\n' << std::flush;
      if (!output)
        throw Failure{1, "OUTPUT_CLOSED", "output stream closed"};
    };
    const auto result = execute(request, emit);
    if (request.command == "freeze" && request.json && !request.ndjson &&
        !result.contains("destination")) {
      errors << encoded({{"code", "CAPTURE_LOSSY"},
                         {"level", "warning"},
                         {"message", "Capture cannot recover original command "
                                     "arguments, history, plugins or before scripts."}})
             << '\n';
    }
    const bool failed = request.command == "load" && result.at("status") != "ok";
    if (failed) {
      for (const auto& error : result.at("errors")) {
        if (request.machine())
          errors << encoded(error) << '\n';
        else
          errors << "Error: " << error.at("message").get<std::string>() << '\n';
      }
    }
    if (request.ndjson) {
      if (request.command == "load")
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
    if (request.machine())
      errors << encoded({{"code", error.code}, {"message", error.what()}}) << '\n';
    else
      errors << "Error: " << error.what() << '\n';
    return error.exit_code;
  } catch (const std::exception& error) {
    if (request.machine())
      errors << encoded({{"code", "OPERATION_FAILED"}, {"message", error.what()}})
             << '\n';
    else
      errors << "Error: " << error.what() << '\n';
    return 1;
  }
}
} // namespace libtmux::workspace::cli
