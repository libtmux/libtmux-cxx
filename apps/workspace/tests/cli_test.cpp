#include "../src/progress.hpp"
#include "../src/services.hpp"
#include "workspace_cli.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <poll.h>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "libtmux/server.hpp"
#include "libtmux/testing/environment_guard.hpp"
#include "libtmux/testing/scoped_server.hpp"
#include "libtmux_consumers/tmuxp.hpp"

namespace {
using Json = nlohmann::json;
struct Result {
  int code;
  std::string out;
  std::string err;
};
Result invoke(std::vector<std::string> args) {
  std::istringstream input;
  std::ostringstream out, err;
  const int code = libtmux::workspace::cli::run(std::move(args), input, out, err);
  return {code, out.str(), err.str()};
}

TEST(WorkspaceCli, CompletionUsesCommandAndValueContextWithoutBackend) {
  libtmux::test::EnvironmentGuard path{"PATH", "/nonexistent-workspace-completion"};
  libtmux::test::EnvironmentGuard progress{"TMUXP_PROGRESS_LINES", "invalid"};
  for (const auto& [words, expected] :
       std::vector<std::pair<std::vector<std::string>, std::string>>{
           {{""}, "values\nload\n"},
           {{"--color", "never", "im"}, "values\nimport\n"},
           {{"import", "tm"}, "values\ntmuxinator\n"},
           {{"load", "--pr"}, "values\n--progress-format\n--progress-lines\n"},
           {{"freeze", "--workspace-format", "j"}, "values\njson\n"},
           {{"freeze", "-fj"}, "values\n-fjson\n"},
           {{"load", "-fproject"}, "files\n-f\n"},
           {{"load", "-?fproject"}, "values\n"},
           {{"freeze", "--save-to=project"}, "files\n--save-to=\n"},
           {{"--color", "=", "al"}, "values\nalways\n"},
           {{"--color=a"}, "values\n--color=auto\n--color=always\n"},
           {{"load", "-S", "im"}, "files\n"},
           {{"load", "--", "--pr"}, "files\n"},
           {{"shell", "--pt"}, "values\n--ptipython\n--ptpython\n"},
           {{"load", "project path", "--no"}, "values\n--no-progress\n"}}) {
    auto arguments = words;
    arguments.insert(arguments.begin(), "--complete");
    const auto result = invoke(std::move(arguments));
    EXPECT_EQ(result.code, 0) << result.err;
    EXPECT_TRUE(result.err.empty());
    if (words == std::vector<std::string>{""})
      EXPECT_TRUE(result.out.starts_with(expected)) << result.out;
    else
      EXPECT_EQ(result.out, expected);
  }
}

TEST(WorkspaceCli, GeneratesNativeCompletionScripts) {
  for (const auto* shell : {"bash", "zsh", "fish"}) {
    const auto result = invoke({"--generate-completion", shell});
    EXPECT_EQ(result.code, 0) << result.err;
    EXPECT_TRUE(result.err.empty());
    EXPECT_NE(result.out.find("--complete"), std::string::npos);
    EXPECT_NE(result.out.find("tmux-workspace"), std::string::npos);
    const auto machine = invoke({"--generate-completion", shell, "--json"});
    ASSERT_EQ(machine.code, 0) << machine.err;
    const auto record = Json::parse(machine.out);
    EXPECT_EQ(record.at("shell"), shell);
    EXPECT_EQ(record.at("script"), result.out);
    const auto stream = invoke({"--generate-completion", shell, "--ndjson"});
    ASSERT_EQ(stream.code, 0) << stream.err;
    const auto completed = Json::parse(stream.out);
    EXPECT_EQ(completed.at("event"), "completed");
    EXPECT_EQ(completed.at("sequence"), 1);
    EXPECT_EQ(completed.at("script"), result.out);
    EXPECT_EQ(completed.at("shell"), shell);
    EXPECT_EQ(std::count(stream.out.begin(), stream.out.end(), '\n'), 1);
  }
  const auto invalid = invoke({"--generate-completion", "unknown", "--json"});
  EXPECT_EQ(invalid.code, 2);
  EXPECT_TRUE(invalid.out.empty());
}

TEST(WorkspaceCli, CompletionReportsClosedOutput) {
  struct Closed : std::streambuf {
    int_type overflow(int_type) override { return traits_type::eof(); }
  };
  for (const bool throwing : {false, true}) {
    Closed buffer;
    std::ostream output{&buffer};
    if (throwing)
      output.exceptions(std::ios::badbit);
    std::istringstream input;
    std::ostringstream errors;
    EXPECT_EQ(libtmux::workspace::cli::run({"--complete", "im"}, input, output, errors),
              1);
    EXPECT_TRUE(errors.str().empty());
  }
}

TEST(WorkspaceCli, CompletionReportsFailedFlush) {
  struct FailedFlush : std::stringbuf {
    int sync() override { return -1; }
  };
  for (const auto& arguments : std::vector<std::vector<std::string>>{
           {"--complete", "im"},
           {"--generate-completion", "bash"},
           {"--generate-completion", "bash", "--json"},
           {"--generate-completion", "bash", "--ndjson"}}) {
    for (const bool throwing : {false, true}) {
      FailedFlush buffer;
      std::ostream output{&buffer};
      if (throwing)
        output.exceptions(std::ios::badbit);
      std::istringstream input;
      std::ostringstream errors;
      EXPECT_EQ(libtmux::workspace::cli::run(arguments, input, output, errors), 1);
    }
  }
}

struct Files {
  std::filesystem::path directory;
  std::filesystem::path previous{std::filesystem::current_path()};
  std::map<std::string, std::optional<std::string>> environment;
  Files() {
    auto pattern =
        (std::filesystem::temp_directory_path() / "cxx-ws-files-XXXXXX").string();
    const char* created = ::mkdtemp(pattern.data());
    if (created == nullptr)
      throw std::runtime_error{"mkdtemp failed"};
    directory = std::filesystem::canonical(created);
    for (const auto* key : {"HOME", "XDG_CONFIG_HOME", "TMUXP_CONFIGDIR"}) {
      const char* value = std::getenv(key);
      environment[key] =
          value == nullptr ? std::nullopt : std::optional<std::string>{value};
      ::setenv(key, directory.c_str(), 1);
    }
    std::filesystem::current_path(directory);
  }
  ~Files() {
    std::filesystem::current_path(previous);
    for (const auto& [key, value] : environment) {
      if (value)
        ::setenv(key.c_str(), value->c_str(), 1);
      else
        ::unsetenv(key.c_str());
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
};

TEST(WorkspaceCli, ShellPreservesArgumentsStreamsAndChildStatus) {
  Files files;
  const auto runtime = files.directory / "tmuxp runtime";
  std::ofstream{runtime}
      << "#!/bin/sh\n"
         "if [ \"$3\" = --version ]; then\n"
         "  printf 'tmuxp 1.74.0, libtmux fixture\\n'; exit 0\n"
         "fi\n"
         "printf '%s\\0' \"$@\" > arguments\n"
         "printf 'shell output'; printf 'shell diagnostic' >&2; exit 7\n";
  ASSERT_EQ(::chmod(runtime.c_str(), 0700), 0);
  libtmux::test::EnvironmentGuard selected{"TMUX_WORKSPACE_TMUXP", runtime.string()};
  const std::string code = "print('literal --json $VALUE; spaced argument')";
  const auto result =
      invoke({"shell", "session name", "window name", "-L", "ignored", "-S",
              "selected socket", "--code", "--use-pythonrc", "--no-startup",
              "--no-vi-mode", "--use-vi-mode", "-c", code, "--json"});
  ASSERT_EQ(result.code, 7) << result.out << result.err;
  const auto value = Json::parse(result.out);
  EXPECT_EQ(value.at("status"), "error");
  EXPECT_EQ(value.at("script_output").at("stdout"), "shell output");
  EXPECT_EQ(value.at("script_output").at("stderr"), "shell diagnostic");
  std::ifstream input{"arguments", std::ios::binary};
  std::vector<std::string> args;
  for (std::string argument; std::getline(input, argument, '\0');)
    args.push_back(std::move(argument));
  EXPECT_NE(std::ranges::find(args, "-c=" + code), args.end());
  EXPECT_NE(std::ranges::find(args, "session name"), args.end());
  EXPECT_NE(std::ranges::find(args, "window name"), args.end());
  EXPECT_NE(std::ranges::find(args, "selected socket"), args.end());
  EXPECT_EQ(std::ranges::find(args, "ignored"), args.end());
  EXPECT_LT(std::ranges::find(args, "--use-pythonrc"),
            std::ranges::find(args, "--no-startup"));
  EXPECT_LT(std::ranges::find(args, "--no-vi-mode"),
            std::ranges::find(args, "--use-vi-mode"));
  EXPECT_EQ(std::ranges::find(args, "--json"), args.end());
  const auto human = invoke({"--color", "always", "shell", "-c", code});
  EXPECT_EQ(human.code, 7);
  EXPECT_EQ(human.out, "shell output");
  EXPECT_EQ(human.err, "shell diagnostic");
  std::ifstream human_arguments{"arguments", std::ios::binary};
  std::string argument;
  for (int index = 0; index < 2; ++index)
    std::getline(human_arguments, argument, '\0');
  EXPECT_EQ(argument, "always");
  const auto stream = invoke({"shell", "-c", "", "--ndjson"});
  ASSERT_EQ(stream.code, 7) << stream.err;
  std::istringstream frames{stream.out};
  std::string stdout_text, stderr_text;
  std::size_t sequence{};
  Json final;
  for (std::string line; std::getline(frames, line);) {
    const auto frame = Json::parse(line);
    EXPECT_EQ(frame.at("sequence"), ++sequence);
    if (frame.at("event") == "script-output")
      (frame.at("stream") == "stdout" ? stdout_text : stderr_text) +=
          frame.at("text").get<std::string>();
    final = frame;
  }
  EXPECT_EQ(stdout_text, "shell output");
  EXPECT_EQ(stderr_text, "shell diagnostic");
  EXPECT_EQ(final.at("event"), "failed");
  EXPECT_EQ(final.at("exit_code"), 7);
}

TEST(WorkspaceCli, ShellRefusesIncompatibleRuntimeBeforeExecution) {
  Files files;
  const auto runtime = files.directory / "wrong runtime";
  std::ofstream{runtime} << "#!/bin/sh\n"
                            "if [ \"$3\" = --version ]; then\n"
                            "  printf 'tmuxp 1.74.00, libtmux fixture\\n'; exit 0\n"
                            "fi\n"
                            "touch executed\n";
  ASSERT_EQ(::chmod(runtime.c_str(), 0700), 0);
  libtmux::test::EnvironmentGuard selected{"TMUX_WORKSPACE_TMUXP", runtime.string()};
  const auto result = invoke({"shell", "-c", "print(1)", "--json"});
  EXPECT_EQ(result.code, 1);
  EXPECT_TRUE(result.out.empty());
  EXPECT_EQ(Json::parse(result.err).at("code"), "compatibility_runtime");
  EXPECT_FALSE(std::filesystem::exists("executed"));
  const auto interactive = invoke({"shell", "--json"});
  EXPECT_EQ(interactive.code, 2);
  EXPECT_EQ(Json::parse(interactive.err).at("code"), "usage");
  libtmux::test::EnvironmentGuard missing{"TMUX_WORKSPACE_TMUXP",
                                          (files.directory / "absent").string()};
  const auto unavailable = invoke({"shell", "-c", "print(1)", "--json"});
  EXPECT_EQ(unavailable.code, 1);
  EXPECT_EQ(Json::parse(unavailable.err).at("code"), "compatibility_runtime");
}

TEST(WorkspaceCli, ShellHonoursTmuxWorkspacePythonOverTmuxp) {
  Files files;
  const auto interpreter = files.directory / "python fixture";
  // Recognise the version probe, else record argv.
  std::ofstream{interpreter} << "#!/bin/sh\n"
                                "if [ \"$6\" = --version ]; then\n"
                                "  printf 'tmuxp 1.74.0, libtmux fixture\\n'; exit 0\n"
                                "fi\n"
                                "printf '%s\\0' \"$@\" > arguments\n"
                                "exit 0\n";
  ASSERT_EQ(::chmod(interpreter.c_str(), 0700), 0);
  libtmux::test::EnvironmentGuard python{"TMUX_WORKSPACE_PYTHON", interpreter.string()};
  // Would fail outright if used, proving the interpreter is what ran.
  libtmux::test::EnvironmentGuard tmuxp{"TMUX_WORKSPACE_TMUXP",
                                        (files.directory / "absent").string()};
  const auto result = invoke({"shell", "-c", "print(1)", "--json"});
  ASSERT_EQ(result.code, 0) << result.err;
  std::ifstream input{"arguments", std::ios::binary};
  std::vector<std::string> args;
  for (std::string argument; std::getline(input, argument, '\0');)
    args.push_back(std::move(argument));
  ASSERT_GE(args.size(), 3U) << result.err;
  EXPECT_EQ(args[0], "-u");
  EXPECT_EQ(args[1], "-c");
  EXPECT_NE(args[2].find("tmuxp.cli"), std::string::npos) << args[2];
  EXPECT_NE(std::ranges::find(args, "-c=print(1)"), args.end());
}

TEST(WorkspaceCli, ShellReportsBothVariablesWhenThePythonInterpreterIsMissing) {
  Files files;
  libtmux::test::EnvironmentGuard python{
      "TMUX_WORKSPACE_PYTHON", (files.directory / "no-such-interpreter").string()};
  const auto result = invoke({"shell", "-c", "print(1)", "--json"});
  EXPECT_EQ(result.code, 1);
  const auto error = Json::parse(result.err);
  EXPECT_EQ(error.at("code"), "compatibility_runtime");
  const auto& message = error.at("message").get_ref<const std::string&>();
  EXPECT_NE(message.find("TMUX_WORKSPACE_PYTHON"), std::string::npos) << message;
}

TEST(WorkspaceCli, ShellRetainsOutputLimitFailure) {
  Files files;
  const auto runtime = files.directory / "large runtime";
  std::ofstream{runtime} << "#!/bin/sh\n"
                            "if [ \"$3\" = --version ]; then\n"
                            "  printf 'tmuxp 1.74.0, libtmux fixture\\n'; exit 0\n"
                            "fi\n"
                            "head -c 1048577 /dev/zero | tr '\\000' x\n";
  ASSERT_EQ(::chmod(runtime.c_str(), 0700), 0);
  libtmux::test::EnvironmentGuard selected{"TMUX_WORKSPACE_TMUXP", runtime.string()};
  const auto result = invoke({"shell", "-c", "", "--ndjson"});
  ASSERT_EQ(result.code, 1);
  EXPECT_EQ(Json::parse(result.err).at("code"), "output_limit");
  std::istringstream frames{result.out};
  Json final;
  int failures{};
  for (std::string line; std::getline(frames, line);) {
    final = Json::parse(line);
    if (final.at("event") == "failed")
      ++failures;
  }
  ASSERT_EQ(failures, 1);
  ASSERT_EQ(final.at("event"), "failed");
  EXPECT_EQ(final.at("exit_code"), 1);
  EXPECT_TRUE(final.at("script_output").at("truncated").get<bool>());
  const auto& retained =
      final.at("script_output").at("stdout").get_ref<const std::string&>();
  EXPECT_FALSE(retained.empty());
  EXPECT_LE(retained.size(), 1024U * 1024U);
}

TEST(WorkspaceCli, HelpAndInvalidRequestsDoNotNeedTmux) {
  const auto help = invoke({"load", "--help"});
  EXPECT_EQ(help.code, 0);
  EXPECT_NE(help.out.find("--json"), std::string::npos);
  EXPECT_NE(help.out.find("--progress-lines"), std::string::npos);
  EXPECT_TRUE(help.err.empty());
  for (const auto& args : std::vector<std::vector<std::string>>{
           {"--json"},
           {"--json", "load"},
           {"import", "teamocil", "--ndjson"},
           {"import", "tmuxinator", "--json"},
           {"load", "file", "-2", "-8", "--json"},
           {"--json", "shell", "--code", "--ipython"},
           {"--json", "load", "file", "--progress-lines", "bad"},
           {"--json", "search"},
           {"--json", "search", "["},
           {"--json", "load", "file", "--unknown"}}) {
    const auto result = invoke(args);
    EXPECT_EQ(result.code, 2);
    EXPECT_TRUE(result.out.empty());
    ASSERT_FALSE(result.err.empty());
    const auto error = Json::parse(result.err);
    EXPECT_TRUE(error.contains("code"));
    EXPECT_TRUE(error.contains("message"));
  }
}

TEST(WorkspaceCli, LegacyColourFailsBeforeDocumentResolution) {
  Files files;
  for (const auto* mode : {"--json", "--ndjson"}) {
    const auto result = invoke({"load", "missing.yaml", "-d", "-8", mode});
    EXPECT_EQ(result.code, 2);
    EXPECT_TRUE(result.out.empty());
    const auto error = Json::parse(result.err);
    EXPECT_EQ(error.at("code"), "usage");
    EXPECT_NE(error.at("message").get<std::string>().find("88-colour"),
              std::string::npos);
  }
}

TEST(WorkspaceCli, OrdinaryLoadRequiresTerminalBeforeDocumentResolution) {
  Files files;
  const auto result = invoke({"load", "missing.yaml"});
  EXPECT_EQ(result.code, 2);
  EXPECT_TRUE(result.out.empty());
  EXPECT_NE(result.err.find("terminal"), std::string::npos) << result.err;
  for (const auto* mode : {"--json", "--ndjson"}) {
    const auto machine = invoke({"load", "missing.yaml", mode});
    EXPECT_EQ(machine.code, 2);
    EXPECT_TRUE(machine.out.empty());
    EXPECT_EQ(Json::parse(machine.err).at("code"), "usage");
  }
}

TEST(WorkspaceCliTmux, MalformedLayoutsPrecedeEveryInputAndBeforeScript) {
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-layout")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto directory = fixture->socket_path().parent_path();
  const auto first = directory / "first.yaml";
  const auto second = directory / "second.yaml";
  const auto marker = directory / "script-ran";
  std::ofstream{first} << "session_name: first\nbefore_script: touch '"
                       << marker.string() << "'\nwindows: [{}]\n";
  std::ofstream{second}
      << "session_name: second\nwindows: [{layout: invalid-layout}]\n";
  const auto result = invoke({"load", first.string(), second.string(), "-d", "-S",
                              fixture->socket_path().string(), "--json"});
  EXPECT_EQ(result.code, 1);
  EXPECT_FALSE(std::filesystem::exists(marker));
  const auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());
  EXPECT_FALSE(server->session("first").has_value());
  EXPECT_FALSE(server->session("second").has_value());
  EXPECT_TRUE(server->session("libtmux_test").has_value());
}

TEST(WorkspaceCliTmux, LayoutCorpusPreservesTmuxCompatibilityAndTheExistingSession) {
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("layouts")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());
  const auto reply = server->run({"display-message", "-p", "#{version}"});
  ASSERT_TRUE(reply.has_value());
  const auto version = libtmux::parse_version("tmux " + *reply);
  ASSERT_TRUE(version.has_value());
  const auto column =
      *version >= libtmux::Version{.major = 3, .minor = 5} ? "3.7c" : "3.2a";
  std::ifstream input{std::filesystem::path{__FILE__}.parent_path() / "fixtures" /
                      "layouts.json"};
  ASSERT_TRUE(input.is_open());
  const auto cases = Json::parse(input);
  const auto config = fixture->socket_path().parent_path() / "layout.json";
  for (const auto& item : cases) {
    SCOPED_TRACE(item.at("id").get<std::string>());
    Json panes = Json::array();
    for (std::size_t index = 0; index < item.at("pane_count").get<std::size_t>();
         ++index)
      panes.push_back("");
    std::ofstream{config} << Json{{"session_name", "layout-corpus"},
                                  {"windows",
                                   Json::array({{{"window_name", "main"},
                                                 {"layout", item.at("layout")},
                                                 {"panes", panes}}})}}
                                 .dump();
    const auto result = invoke({"load", config.string(), "-d", "-S",
                                fixture->socket_path().string(), "--json"});
    const bool valid = item.at("expected_valid").at(column).get<bool>();
    EXPECT_EQ(result.code, valid ? 0 : 1) << result.out << result.err;
    const auto session = server->session("layout-corpus");
    EXPECT_EQ(session.has_value(), valid);
    if (session) {
      ASSERT_TRUE(session->kill().has_value());
    }
    ASSERT_TRUE(server->session("libtmux_test").has_value());
  }
}

TEST(WorkspaceCliTmux, BeforeScriptsValidateEveryInputBeforeMutation) {
  Files files;
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-script")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());
  std::ofstream{"first.json"} << Json{{"session_name", "script-first"},
                                      {"windows", Json::array({Json::object()})}};
  const Json valid{{"session_name", "script-invalid"},
                   {"before_script", "/bin/sh -c 'printf ran > ran-script'"},
                   {"windows", Json::array({Json::object()})}};
  for (const auto& invalid : std::vector<Json>{
           {{"before_script", Json::object()}},
           {{"before_script", "'/bin/true"}},
           {{"before_script", std::string{"/bin/true\0hidden", 16}}},
           {{"start_directory", "missing-directory"}},
           {{"environment", {{"", "invalid"}}}},
           {{"environment", {{"A=B", "invalid"}}}},
           {{"windows", Json::array({{{"environment", {{"A=B", "invalid"}}}}})}},
           {{"windows",
             Json::array({{{"panes", Json::array({{{"environment",
                                                    {{"", "invalid"}}}}})}}})}}}) {
    auto document = valid;
    document.update(invalid);
    std::ofstream{"second.json"} << document;
    const auto result = invoke({"load", "first.json", "second.json", "-d", "-S",
                                fixture->socket_path().string(), "--json"});
    EXPECT_EQ(result.code, 1) << result.out << result.err;
    EXPECT_TRUE(result.out.empty());
    EXPECT_FALSE(server->session("=script-first:").has_value());
    EXPECT_FALSE(server->session("=script-invalid:").has_value());
    EXPECT_FALSE(std::filesystem::exists("ran-script"));
  }
}

TEST(WorkspaceCliTmux, LogFilesFilterRecordsWithoutChangingResults) {
  Files files;
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-log")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());
  for (const auto* level : {"info", "debug", "critical"}) {
    for (const bool failed : {false, true}) {
      const auto name = std::string{level} + (failed ? "-failed" : "-success");
      SCOPED_TRACE(name);
      std::ofstream{"logged.json"} << Json{
          {"session_name", name},
          {"before_script",
           std::string{"/bin/sh -c 'printf captured-out; printf captured-err >&2; "} +
               (failed ? "kill -TERM $$'" : "exit 0'")},
          {"windows", Json::array({Json::object()})}};
      const auto path = name + ".log";
      const bool existing = std::string_view{level} == "info";
      if (existing) {
        std::ofstream{path} << "retained\n";
        ASSERT_EQ(::chmod(path.c_str(), 0640), 0);
      }
      const auto result =
          invoke({"--log-level", level, "load", "logged.json", "-d", "-S",
                  fixture->socket_path().string(), "--json", "--log-file", path});
      ASSERT_EQ(result.code, failed ? 143 : 0) << result.err;
      EXPECT_EQ(server->session("=" + name + ":").has_value(), !failed);
      const auto summary = Json::parse(result.out);
      const auto& item = summary.at(failed ? "errors" : "results")[0];
      EXPECT_EQ(item.at("script_output").at("stdout"), "captured-out");
      EXPECT_EQ(item.at("script_output").at("stderr"), "captured-err");
      if (failed)
        EXPECT_EQ(Json::parse(result.err).at("code"), "script_failed");
      else
        EXPECT_TRUE(result.err.empty());
      std::ifstream file{path};
      EXPECT_TRUE(file.is_open()) << "log file was not created";
      if (!file)
        continue;
      struct stat metadata {};
      ASSERT_EQ(::stat(path.c_str(), &metadata), 0);
      EXPECT_EQ(metadata.st_mode & 0777, existing ? 0640 : 0600);
      std::string line;
      if (existing) {
        ASSERT_TRUE(static_cast<bool>(std::getline(file, line)));
        EXPECT_EQ(line, "retained");
      }
      bool completed{}, stdout_record{}, stderr_record{};
      int records{};
      while (std::getline(file, line)) {
        ++records;
        const auto record = Json::parse(line);
        EXPECT_EQ(record.at("command"), "load");
        const auto event = record.value("event", "");
        completed |= event == (failed ? "failed" : "completed");
        if (event == "script-completed") {
          EXPECT_EQ(record.at("data").at("exit_code"), failed ? 143 : 0);
          EXPECT_EQ(record.at("data").at("truncated"), false);
        }
        if (event == "script-output") {
          EXPECT_EQ(record.at("severity"), "debug");
          stdout_record |= record.at("data").at("stream") == "stdout";
          stderr_record |= record.at("data").at("stream") == "stderr";
        } else {
          EXPECT_EQ(line.find("captured-out"), std::string::npos);
          EXPECT_EQ(line.find("captured-err"), std::string::npos);
          EXPECT_EQ(line.find("script_output"), std::string::npos);
        }
      }
      EXPECT_EQ(completed, std::string_view{level} != "critical");
      EXPECT_EQ(stdout_record && stderr_record, std::string_view{level} == "debug");
      if (std::string_view{level} == "critical") {
        EXPECT_EQ(records, 0);
      }
    }
  }
}

TEST(WorkspaceCliTmux, LogFileRefusalPrecedesMutation) {
  Files files;
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-log")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());
  std::ofstream{"logged.yaml"} << "session_name: blocked\nwindows: [{}]\n";
  std::ofstream{"regular"} << "untouched";
  std::filesystem::create_directory("directory");
  std::filesystem::create_symlink("regular", "link");
  ASSERT_EQ(::mkfifo("fifo", 0600), 0);
  for (const auto* path : {"missing/log", "directory", "link", "fifo", "/dev/null"}) {
    SCOPED_TRACE(path);
    const auto result =
        invoke({"--log-level", "critical", "load", "logged.yaml", "-d", "-S",
                fixture->socket_path().string(), "--json", "--log-file", path});
    EXPECT_EQ(result.code, 1);
    EXPECT_TRUE(result.out.empty());
    EXPECT_NE(result.err.find("log_file_unavailable"), std::string::npos);
    const auto created = server->session("=blocked:");
    EXPECT_FALSE(created.has_value());
    if (created) {
      ASSERT_TRUE(created->kill().has_value());
    }
  }
  std::string retained;
  std::ifstream{"regular"} >> retained;
  EXPECT_EQ(retained, "untouched");
  EXPECT_EQ(invoke({"load", "--log-file", "missing/log", "--help"}).code, 0);
  const auto invalid = invoke({"--log-level", "invalid", "load", "logged.yaml",
                               "--log-file", "unexpected.log", "--json"});
  EXPECT_EQ(invalid.code, 2);
  EXPECT_FALSE(std::filesystem::exists("unexpected.log"));
}

TEST(WorkspaceCliTmux, LogLevelFiltersOnlyOptionalDiagnostics) {
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-log")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  for (const auto* level : {"warning", "critical"}) {
    const auto result =
        invoke({"--log-level", level, "freeze", std::string{fixture->session_name()},
                "-S", fixture->socket_path().string(), "--json"});
    EXPECT_EQ(result.code, 0);
    EXPECT_FALSE(result.out.empty());
    EXPECT_EQ(result.err.empty(), std::string_view{level} == "critical");
  }
  const auto failed = invoke({"--log-level", "critical", "freeze", "=absent:", "-S",
                              fixture->socket_path().string(), "--json"});
  EXPECT_EQ(failed.code, 1);
  EXPECT_FALSE(failed.err.empty());
}

TEST(WorkspaceCliTmux, BeforeScriptsRetainOutputAndRespectSessionOwnership) {
  Files files;
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-script")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());
  std::filesystem::create_directory("work");
  std::ofstream{"work/before.sh"}
      << "tmux -S \"$2\" has-session -t '=script-success:' || exit 9\n"
         "tmux -S \"$2\" show-environment -t '=script-success:' SCRIPT_AFTER "
         ">/dev/null 2>&1 && exit 8\n"
         "printf '%s|%s' \"$PWD\" \"$1\"\nprintf warning >&2\n";
  Json document{
      {"session_name", "script-success"},
      {"start_directory", "work"},
      {"before_script", "/bin/sh before.sh 'literal $VALUE; touch unwanted' '" +
                            fixture->socket_path().string() + "'"},
      {"environment", {{"SCRIPT_AFTER", "later"}}},
      {"windows", Json::array({Json::object()})}};
  std::ofstream{"script.json"} << document;
  const auto loaded = invoke(
      {"load", "script.json", "-d", "-S", fixture->socket_path().string(), "--json"});
  ASSERT_EQ(loaded.code, 0) << loaded.err << loaded.out;
  const auto summary = Json::parse(loaded.out);
  ASSERT_TRUE(summary.at("results")[0].contains("script_output"));
  EXPECT_EQ(summary.at("results")[0].at("script_output").at("stdout"),
            (files.directory / "work").string() + "|literal $VALUE; touch unwanted");
  EXPECT_EQ(summary.at("results")[0].at("script_output").at("stderr"), "warning");
  EXPECT_FALSE(std::filesystem::exists("work/unwanted"));
  const auto session = server->session("=script-success:");
  ASSERT_TRUE(session.has_value());
  const auto pane = session->active_pane();
  ASSERT_TRUE(pane.has_value());
  const auto pid = pane->expand("#{pid}");
  ASSERT_TRUE(pid.has_value());
  std::ofstream{"work/fail.sh"} << "printf failed\nprintf detail >&2\nexit 7\n";
  document["session_name"] = "script-failed";
  document["before_script"] = "/bin/sh fail.sh";
  std::ofstream{"script.json"} << document;
  const auto failed = invoke(
      {"load", "script.json", "-d", "-S", fixture->socket_path().string(), "--json"});
  ASSERT_EQ(failed.code, 1) << failed.err << failed.out;
  const auto problem = Json::parse(failed.out).at("errors")[0];
  EXPECT_EQ(problem.at("failed_stage"), "before-script");
  EXPECT_EQ(problem.at("script_output").at("exit_code"), 7);
  EXPECT_EQ(problem.at("script_output").at("stdout"), "failed");
  EXPECT_FALSE(server->session("=script-failed:").has_value());
  libtmux::test::EnvironmentGuard tmux{"TMUX", fixture->socket_path().string() + "," +
                                                   *pid + ",0"};
  libtmux::test::EnvironmentGuard current_pane{"TMUX_PANE", pane->id()};
  const auto appended = invoke({"load", "script.json", "--append", "--json"});
  ASSERT_EQ(appended.code, 1) << appended.err << appended.out;
  const auto partial = Json::parse(appended.out);
  EXPECT_EQ(partial.at("status"), "partial");
  EXPECT_EQ(partial.at("errors")[0].at("retained_state").at("session_id"),
            session->id());
  EXPECT_EQ(partial.at("errors")[0].at("script_output").at("stderr"), "detail");
  ASSERT_TRUE(session->windows().has_value());
  EXPECT_EQ(session->windows()->size(), 1U);
  EXPECT_TRUE(pane->expand("#{pane_id}").has_value());
}

TEST(WorkspaceCli, FileServicesKeepTypesAndUseNativeWholeWordMatching) {
  Files files;
  std::ofstream{"dev.yaml"}
      << "session_name: developer\ncustom: {enabled: true, count: 3}\n"
         "windows: [{window_name: editor, panes: [echo marker]}]\n";
  const auto list = invoke({"--json", "ls", "--full"});
  ASSERT_EQ(list.code, 0) << list.err;
  const auto records = Json::parse(list.out).at("workspaces");
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(records[0].at("config").at("custom").at("count"), 3);
  const auto grouped = invoke({"search", "session:dev|ops", "-w", "--json"});
  ASSERT_EQ(grouped.code, 0) << grouped.err;
  EXPECT_TRUE(Json::parse(grouped.out).empty());
  const auto found = invoke({"search", "pane:marker", "--json"});
  ASSERT_EQ(found.code, 0) << found.err;
  const auto matches = Json::parse(found.out);
  ASSERT_EQ(matches.size(), 1U);
  EXPECT_EQ(matches[0].at("matched_fields"), Json::array({"pane"}));
  const auto repeated = invoke({"search", "pane:marker", "pane:marker", "--json"});
  ASSERT_EQ(repeated.code, 0) << repeated.err;
  EXPECT_EQ(Json::parse(repeated.out)[0].at("matches").at("pane").size(), 1U);
  const auto converted = invoke({"convert", "dev.yaml", "--json"});
  ASSERT_EQ(converted.code, 0) << converted.err;
  EXPECT_EQ(Json::parse(converted.out).at("custom").at("enabled"), true);
  const auto preview = invoke({"convert", "dev.yaml"});
  EXPECT_EQ(preview.code, 0);
  EXPECT_FALSE(std::filesystem::exists(files.directory / "dev.json"));
  const auto saved =
      invoke({"convert", "dev.yaml", "--save-to", "saved.json", "--json"});
  ASSERT_EQ(saved.code, 0) << saved.err;
  const auto refused =
      invoke({"convert", "dev.yaml", "--save-to", "saved.json", "--json"});
  EXPECT_EQ(refused.code, 1);
  EXPECT_TRUE(refused.out.empty());
  EXPECT_EQ(Json::parse(refused.err).at("code"), "destination_exists");
  std::ofstream{"old.yml"}
      << "name: imported\nroot: /tmp\nwindows: [{main: [echo hello]}]\n";
  const auto tmuxinator = invoke({"import", "tmuxinator", "old.yml", "--ndjson"});
  ASSERT_EQ(tmuxinator.code, 0) << tmuxinator.err;
  EXPECT_EQ(
      Json::parse(tmuxinator.out).at("workspace").at("windows")[0].at("window_name"),
      "main");
  std::ofstream{"team.yml"} << "session: {name: team, windows: [{name: work, splits: "
                               "[{cmd: echo hello}]}]}\n";
  const auto teamocil = invoke({"--json", "import", "teamocil", "team.yml"});
  ASSERT_EQ(teamocil.code, 0) << teamocil.err;
  EXPECT_EQ(
      Json::parse(teamocil.out).at("windows")[0].at("panes")[0].at("shell_command"),
      "echo hello");
  const auto debug = invoke({"debug-info", "--json"});
  ASSERT_EQ(debug.code, 0) << debug.err;
  EXPECT_EQ(Json::parse(debug.out).at("port"), "cxx");
}

TEST(WorkspaceCli, VersionNamesTheToolAndJsonOutputIsOneCompactLine) {
  const auto version = invoke({"--version"});
  EXPECT_EQ(version.code, 0);
  EXPECT_TRUE(version.out.starts_with("tmux-workspace ")) << version.out;
  EXPECT_GT(version.out.size(), std::string{"tmux-workspace \n"}.size()) << version.out;

  Files files;
  std::ofstream{"dev.yaml"} << "session_name: developer\nwindows: [{}]\n";
  const auto converted = invoke({"convert", "dev.yaml", "--json"});
  ASSERT_EQ(converted.code, 0) << converted.err;
  // One machine record per line: a trailing newline and nothing else.
  EXPECT_EQ(converted.out.find('\n'), converted.out.size() - 1) << converted.out;

  // Human mode never emits JSON; `--json` is what keeps the object.
  const auto human = invoke({"--color", "never", "debug-info"});
  ASSERT_EQ(human.code, 0) << human.err;
  EXPECT_FALSE(Json::accept(human.out)) << human.out;
  EXPECT_NE(human.out.find("port: cxx\n"), std::string::npos) << human.out;

  const auto machine = invoke({"debug-info", "--json"});
  ASSERT_EQ(machine.code, 0) << machine.err;
  EXPECT_EQ(Json::parse(machine.out).at("port"), "cxx");
  EXPECT_EQ(machine.out.find('\n'), machine.out.size() - 1) << machine.out;
}

TEST(WorkspaceCli, ListingGroupsDirectoriesAndIncludesFullDocuments) {
  Files files;
  // Only one global directory is ever active; exercise the legacy
  // ~/.tmuxp one by clearing the higher-precedence candidates Files() sets.
  ::unsetenv("TMUXP_CONFIGDIR");
  ::unsetenv("XDG_CONFIG_HOME");
  std::filesystem::create_directory(".tmuxp");
  const Json document{{"session_name", "日本語"},
                      {"custom", {{"enabled", true}, {"count", 3}}},
                      {"windows", Json::array({Json::object()})}};
  for (const auto* path : {".tmuxp.json", ".tmuxp/alpha.json", ".tmuxp/beta.json"})
    std::ofstream{path} << document;
  const std::string flat = ".tmuxp  ~/.tmuxp.json\n"
                           "alpha  ~/.tmuxp/alpha.json\n"
                           "beta  ~/.tmuxp/beta.json\n";
  const std::string tree = "~\n"
                           "└── .tmuxp  ~/.tmuxp.json\n"
                           "~/.tmuxp\n"
                           "├── alpha  ~/.tmuxp/alpha.json\n"
                           "└── beta  ~/.tmuxp/beta.json\n";
  for (const bool grouped : {false, true}) {
    std::vector<std::string> arguments{"--color", "never", "ls"};
    if (grouped)
      arguments.emplace_back("--tree");
    const auto listed = invoke(arguments);
    ASSERT_EQ(listed.code, 0) << listed.err;
    EXPECT_EQ(listed.out, grouped ? tree : flat);
    arguments.emplace_back("--full");
    const auto full = invoke(arguments);
    ASSERT_EQ(full.code, 0) << full.err;
    EXPECT_NE(full.out.find("\"session_name\": \"日本語\""), std::string::npos);
    EXPECT_NE(full.out.find("\"count\": 3"), std::string::npos);
    EXPECT_NE(full.out.find("\"enabled\": true"), std::string::npos);
    if (grouped) {
      EXPECT_NE(full.out.find("├── alpha  ~/.tmuxp/alpha.json\n│     {\n"),
                std::string::npos);
      EXPECT_NE(full.out.find("└── beta  ~/.tmuxp/beta.json\n      {\n"),
                std::string::npos);
    }
  }
}

TEST(WorkspaceCli, ListingEscapesHumanControlsWithoutChangingMachineValues) {
  Files files;
  const std::string name = "日本語\n\r\t\033[31m\177\xc2\x9b";
  const std::string safe = "日本語\\n\\r\\t\\u001b[31m\\u007f\\u009b";
  const Json document{{"session_name", name},
                      {"windows", Json::array({Json::object()})}};
  std::ofstream{name + ".json"} << document;
  for (const bool tree : {false, true}) {
    for (const bool full : {false, true}) {
      std::vector<std::string> arguments{"--color", "never", "ls"};
      if (tree)
        arguments.emplace_back("--tree");
      if (full)
        arguments.emplace_back("--full");
      const auto listed = invoke(arguments);
      ASSERT_EQ(listed.code, 0) << listed.err;
      EXPECT_NE(listed.out.find(safe), std::string::npos);
      EXPECT_EQ(listed.out.find('\033'), std::string::npos);
      EXPECT_EQ(listed.out.find('\r'), std::string::npos);
      EXPECT_EQ(listed.out.find('\t'), std::string::npos);
      EXPECT_EQ(listed.out.find('\177'), std::string::npos);
      EXPECT_EQ(listed.out.find("\xc2\x9b"), std::string::npos);
      for (const auto* mode : {"--json", "--ndjson"}) {
        auto machine_arguments = arguments;
        machine_arguments.emplace_back(mode);
        const auto machine = invoke(machine_arguments);
        ASSERT_EQ(machine.code, 0) << machine.err;
        const auto payload = Json::parse(machine.out);
        const auto row = std::string_view{mode} == "--json"
                             ? payload.at("workspaces").at(0)
                             : payload;
        EXPECT_EQ(row.at("name"), name);
        EXPECT_EQ(row.at("path"), "~/" + name + ".json");
        EXPECT_EQ(row.contains("config"), full);
        if (full) {
          EXPECT_EQ(row.at("config"), document);
        }
      }
    }
  }
}

TEST(WorkspaceCli, ListingFullShowsUnreadableDocumentsAndEmptyTrees) {
  Files files;
  const auto empty = invoke({"--color", "never", "ls", "--tree", "--full"});
  ASSERT_EQ(empty.code, 0) << empty.err;
  EXPECT_TRUE(empty.out.empty());
  std::ofstream{"broken.json"} << "{";
  const auto broken = invoke({"--color", "never", "ls", "--full"});
  ASSERT_EQ(broken.code, 0) << broken.err;
  EXPECT_EQ(broken.out, "broken  ~/broken.json\n  null\n");
}

TEST(WorkspaceCli, ImportTeamocilPreservesModernPaneCommandsAndWindowSettings) {
  Files files;
  std::ofstream{"team.yml"}
      << "name: imported\nwindows:\n"
         "  - name: editor\n    focus: true\n"
         "    options: {'@import-source': teamocil}\n"
         "    panes:\n      - commands: [cd /tmp, printf ready, 'echo <%= literal "
         "%>']\n        focus: true\n";
  const auto imported = invoke({"import", "teamocil", "team.yml", "--json"});
  ASSERT_EQ(imported.code, 0) << imported.err;
  const auto parsed = libtmux::workspace::parse_tmuxp(imported.out);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().where << parsed.error().reason;
  ASSERT_EQ(parsed->windows.size(), 1U);
  const auto& window = parsed->windows[0];
  EXPECT_TRUE(window.focus);
  ASSERT_EQ(window.options.size(), 1U);
  EXPECT_EQ(window.options[0],
            (std::pair<std::string, std::string>{"@import-source", "teamocil"}));
  ASSERT_EQ(window.panes.size(), 1U);
  EXPECT_TRUE(window.panes[0].focus);
  ASSERT_EQ(window.panes[0].shell_commands.size(), 1U);
  // Teamocil evaluates no templates, so this markup is ordinary text and
  // must survive the import.
  EXPECT_EQ(window.panes[0].shell_commands[0].text,
            "cd /tmp; printf ready; echo <%= literal %>");
}

TEST(WorkspaceCli, ImportTmuxinatorKeepsWindowCommandArraysInOnePane) {
  Files files;
  std::ofstream{"project.yml"}
      << "name: imported\nwindows: [{editor: [cd /tmp, printf ready]}]\n";
  const auto imported = invoke({"import", "tmuxinator", "project.yml", "--json"});
  ASSERT_EQ(imported.code, 0) << imported.err;
  const auto parsed = libtmux::workspace::parse_tmuxp(imported.out);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().where << parsed.error().reason;
  ASSERT_EQ(parsed->windows.size(), 1U);
  ASSERT_EQ(parsed->windows[0].panes.size(), 1U);
  const auto& commands = parsed->windows[0].panes[0].shell_commands;
  ASSERT_EQ(commands.size(), 2U);
  EXPECT_EQ(commands[0].text, "cd /tmp");
  EXPECT_EQ(commands[1].text, "printf ready");
}

TEST(WorkspaceCli, ImportRefusesUnsupportedBehaviourBeforeSaving) {
  Files files;
  for (const auto& [source, field] : std::vector<std::pair<std::string, std::string>>{
           {"name: imported\npre: echo host\nwindows: [{editor: echo pane}]\n", "pre"},
           {"name: imported\nwindows: [{editor: {panes: [{title: [echo pane]}]}}]\n",
            "panes[0]"},
           {"name: imported\nroot: '<%= ENV[\"ROOT\"] %>'\nwindows: [{editor: null}]\n",
            "ERB"},
           {"name: imported\nwindows: [{editor: 'echo <%= dynamic_command %>'}]\n",
            "ERB"},
           {"name: imported\nwindows: [{'<%= dynamic_window %>': null}]\n", "ERB"},
           {"name: imported\nstartup_window: editor\nwindows: [{editor: null}]\n",
            "startup_window"}}) {
    std::ofstream{"project.yml"} << source;
    std::ofstream{"saved.json"} << "preserved";
    const auto result = invoke({"import", "tmuxinator", "project.yml", "--save-to",
                                "saved.json", "--force", "--json"});
    EXPECT_EQ(result.code, 1) << result.out << result.err;
    EXPECT_TRUE(result.out.empty());
    EXPECT_NE(result.err.find(field), std::string::npos) << result.err;
    std::ifstream saved{"saved.json"};
    EXPECT_EQ((std::string{std::istreambuf_iterator<char>{saved}, {}}), "preserved");
  }
}

TEST(WorkspaceCli, ImportPreservesCommandGroupingAndSourceDirectory) {
  Files files;
  std::filesystem::create_directory("configs");
  std::ofstream{"configs/project.yml"}
      << "name: imported\nroot: project\npre_window: [cd /tmp, printf global]\n"
         "windows: [{editor: {root: src, pre: ['false', printf local], panes: [echo "
         "pane]}}]\n";
  const auto result = invoke({"import", "tmuxinator", "configs/project.yml", "--json"});
  ASSERT_EQ(result.code, 0) << result.err;
  const auto document = Json::parse(result.out);
  EXPECT_EQ(document.at("start_directory"), (files.directory / "project").string());
  EXPECT_EQ(document.at("shell_command_before"), "cd /tmp; printf global");
  EXPECT_EQ(document.at("windows")[0].at("start_directory"),
            (files.directory / "project/src").string());
  EXPECT_EQ(document.at("windows")[0].at("shell_command_before"),
            "false && printf local");
}

TEST(WorkspaceCli, ImportTeamocilUsesTheFilenameForAnUnnamedSession) {
  Files files;
  std::ofstream{"team.yml"} << "windows: [{name: editor, panes: [echo ready]}]\n";
  const auto result = invoke({"import", "teamocil", "team.yml", "--json"});
  ASSERT_EQ(result.code, 0) << result.err;
  const auto parsed = libtmux::workspace::parse_tmuxp(result.out);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().where << parsed.error().reason;
  EXPECT_EQ(parsed->session_name, "team");
}

TEST(WorkspaceCli, ImportRefusesWindowPreWithoutExplicitPanesBeforeSaving) {
  Files files;
  for (const auto* panes : {"", ", panes: null", ", panes: []"}) {
    std::ofstream{"project.yml"}
        << "name: imported\nwindows: [{work: {pre: echo unexpected" << panes << "}}]\n";
    std::ofstream{"saved.json"} << "preserved";
    const auto result = invoke({"import", "tmuxinator", "project.yml", "--save-to",
                                "saved.json", "--force", "--json"});
    EXPECT_EQ(result.code, 1) << panes << result.out << result.err;
    EXPECT_TRUE(result.out.empty());
    EXPECT_NE(result.err.find("windows[0].pre"), std::string::npos) << result.err;
    std::ifstream saved{"saved.json"};
    EXPECT_EQ((std::string{std::istreambuf_iterator<char>{saved}, {}}), "preserved");
  }
}

TEST(WorkspaceCli, ImportTeamocilKeepsTheFirstFocusInEachScope) {
  Files files;
  std::ofstream{"team.yml"}
      << "name: imported\nwindows:\n"
         "  - name: first\n    focus: true\n"
         "    panes: [{commands: ':', focus: true}, {commands: ':', focus: true}]\n"
         "  - name: second\n    focus: true\n"
         "    panes: [{commands: ':', focus: true}, {commands: ':', focus: true}]\n";
  const auto result = invoke({"import", "teamocil", "team.yml", "--json"});
  ASSERT_EQ(result.code, 0) << result.err;
  const auto parsed = libtmux::workspace::parse_tmuxp(result.out);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().where << parsed.error().reason;
  ASSERT_EQ(parsed->windows.size(), 2U);
  EXPECT_TRUE(parsed->windows[0].focus);
  EXPECT_FALSE(parsed->windows[1].focus);
  for (const auto& window : parsed->windows) {
    ASSERT_EQ(window.panes.size(), 2U);
    EXPECT_TRUE(window.panes[0].focus);
    EXPECT_FALSE(window.panes[1].focus);
  }
}

TEST(WorkspaceCli, ImportRejectsChangedSynchronizationTimingAndScalarTypes) {
  Files files;
  for (const auto& [kind, source] : std::vector<std::pair<std::string, std::string>>{
           {"tmuxinator",
            "name: bad\nwindows: [{work: {synchronize: before, panes: [':', ':']}}]\n"},
           {"teamocil", "name: bad\nwindows: [{name: work, options: "
                        "{synchronize-panes: true}, panes: [':', ':']}]\n"},
           {"tmuxinator", "name: 42\nwindows: [{work: ':'}]\n"},
           {"tmuxinator", "name: bad\nwindows: [{work: 42}]\n"},
           {"teamocil", "name: bad\nwindows: [{name: 42, panes: [':']}]\n"},
           {"teamocil",
            "name: bad\nwindows: [{name: work, panes: [{commands: [true]}]}]\n"},
           {"teamocil",
            "name: bad\nwindows: [{name: work, focus: 'true', panes: [':']}]\n"},
           {"teamocil",
            "name: bad\nwindows: [{name: work, layout: 42, panes: [':']}]\n"}}) {
    std::ofstream{"source.yml"} << source;
    const auto result = invoke({"import", kind, "source.yml", "--json"});
    EXPECT_EQ(result.code, 1) << source << result.out << result.err;
    EXPECT_TRUE(result.out.empty());
    EXPECT_FALSE(result.err.empty());
  }
}

TEST(WorkspaceCli, ImportUsesNonNullAliasesAndRefusesConflicts) {
  Files files;
  std::ofstream{"source.yml"}
      << "project_name: null\nname: expected\nproject_root: null\nroot: /tmp\n"
         "tabs: null\nwindows: [{work: ':'}]\n";
  const auto accepted = invoke({"import", "tmuxinator", "source.yml", "--json"});
  ASSERT_EQ(accepted.code, 0) << accepted.err;
  const auto parsed = libtmux::workspace::parse_tmuxp(accepted.out);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().where << parsed.error().reason;
  EXPECT_EQ(parsed->session_name, "expected");
  EXPECT_EQ(parsed->start_directory, "/tmp");
  for (const auto& [kind, source] : std::vector<std::pair<std::string, std::string>>{
           {"tmuxinator", "project_name: one\nname: two\nwindows: [{work: ':'}]\n"},
           {"tmuxinator",
            "name: bad\nproject_root: /one\nroot: /two\nwindows: [{work: ':'}]\n"},
           {"tmuxinator", "name: bad\ntabs: [{old: ':'}]\nwindows: [{new: ':'}]\n"},
           {"teamocil",
            "name: bad\nwindows: [{name: work, panes: [':'], splits: [echo wrong]}]\n"},
           {"teamocil", "name: bad\nwindows: [{name: work, panes: [{commands: [echo "
                        "one], cmd: echo two}]}]\n"}}) {
    std::ofstream{"source.yml"} << source;
    const auto refused = invoke({"import", kind, "source.yml", "--json"});
    EXPECT_EQ(refused.code, 1) << source << refused.out;
    EXPECT_TRUE(refused.out.empty());
    EXPECT_NE(refused.err.find("conflict"), std::string::npos) << refused.err;
  }
}

// A key starting with "x-", at any level, is inert -- accepted, ignored
// at load, and its refusal message (for every other unknown key) suggests
// the prefix. Also covers the empty `where` a document-level refusal used
// to leave in "Error: : unsupported key: ...".
TEST(WorkspaceCli, ParseTmuxpIgnoresExtensionKeysAtEveryLevel) {
  const auto* text = R"({
    "session_name": "x-test",
    "x-doc-extra": {"anything": true},
    "windows": [
      {"window_name": "a", "x-window-extra": 1, "panes": [
        {"shell_command": "echo a", "x-pane-extra": 2}
      ]}
    ]
  })";
  const auto parsed = libtmux::workspace::parse_tmuxp(text);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().where << ": " << parsed.error().reason;
  EXPECT_EQ(parsed->session_name, "x-test");
  ASSERT_EQ(parsed->windows.size(), 1U);
  EXPECT_EQ(parsed->windows[0].name, "a");

  const auto refused = libtmux::workspace::parse_tmuxp(
      R"({"session_name":"s","bogus":1,"windows":[{"panes":[":"]}]})");
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().where, "");
  EXPECT_NE(refused.error().reason.find("x-"), std::string::npos) << refused.error().reason;
}

TEST(WorkspaceCli, ConvertPreservesExtensionKeysUnread) {
  Files files;
  std::ofstream{"ext.yaml"}
      << "session_name: ext-test\nx-shared: &shared\n  shell_command_before: "
         "[export QA_ANCHOR=1]\nwindows:\n  - window_name: a\n    panes: [echo a]\n";
  const auto converted = invoke({"convert", "ext.yaml", "--json"});
  ASSERT_EQ(converted.code, 0) << converted.err;
  EXPECT_TRUE(Json::parse(converted.out).contains("x-shared"));
}

TEST(WorkspaceCli, LoadRefusalOfATopLevelKeyIsOneCleanSentence) {
  Files files;
  std::ofstream{"bogus.yaml"} << "session_name: s\nbogus: 1\nwindows: [{}]\n";
  const auto refused = invoke({"load", "bogus.yaml", "-d"});
  EXPECT_NE(refused.code, 0);
  EXPECT_EQ(refused.err.find("Error: :"), std::string::npos) << refused.err;
  EXPECT_NE(refused.err.find("unsupported key: bogus"), std::string::npos) << refused.err;
  EXPECT_NE(refused.err.find("x-"), std::string::npos) << refused.err;
}

// Every port's machine error `code` for the same condition used to be
// different. These four conditions -- a missing workspace file, a malformed
// document, a refused key and a freeze target that doesn't exist -- now
// report the shared lower snake_case vocabulary, and every stderr error
// record carries "schema_version":1.
TEST(WorkspaceCliTmux, ErrorCodesMatchTheSharedLowerSnakeCaseVocabulary) {
  Files files;
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-codes")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto socket = fixture->socket_path().string();

  const auto missing = invoke({"load", "missing.yaml", "-d", "-S", socket, "--json"});
  EXPECT_NE(missing.code, 0);
  EXPECT_TRUE(missing.out.empty());
  auto record = Json::parse(missing.err);
  EXPECT_EQ(record.at("schema_version"), 1);
  EXPECT_EQ(record.at("code"), "workspace_not_found");

  std::ofstream{"malformed.yaml"} << "a: [\n";
  const auto malformed =
      invoke({"load", "malformed.yaml", "-d", "-S", socket, "--json"});
  EXPECT_NE(malformed.code, 0);
  record = Json::parse(malformed.err);
  EXPECT_EQ(record.at("schema_version"), 1);
  EXPECT_EQ(record.at("code"), "invalid_workspace");

  std::ofstream{"bogus.yaml"} << "session_name: s\nbogus: 1\nwindows: [{}]\n";
  const auto bogus = invoke({"load", "bogus.yaml", "-d", "-S", socket, "--json"});
  EXPECT_NE(bogus.code, 0);
  record = Json::parse(bogus.err);
  EXPECT_EQ(record.at("schema_version"), 1);
  EXPECT_EQ(record.at("code"), "unsupported_key");

  const auto frozen = invoke({"freeze", "nosuch", "-S", socket, "--json"});
  EXPECT_NE(frozen.code, 0);
  record = Json::parse(frozen.err);
  EXPECT_EQ(record.at("schema_version"), 1);
  EXPECT_EQ(record.at("code"), "session_not_found");
}

// The remaining conditions each get their own server: chaining several tmux
// mutations onto one server before reaching a spawn-failure case
// (before_script) was unreliable in CI.
TEST(WorkspaceCliTmux, RemainingErrorCodesMatchTheSharedVocabulary) {
  Files files;
  std::ofstream{"ok.yaml"} << "session_name: ok\nwindows: [{panes: [echo]}]\n";
  std::ofstream{"tf.yaml"}
      << "session_name: cf\nwindows:\n  - window_name: w\n    options:\n"
         "      no-such-option-xyz: 1\n    panes: [echo]\n";
  std::ofstream{"sf.yaml"}
      << "session_name: cs\nbefore_script: /bin/false\nwindows: [{panes: [echo]}]\n";

  {
    auto fixture = libtmux::test::ScopedTmuxServer::start(
        {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-codes-tf")});
    ASSERT_TRUE(fixture.has_value()) << fixture.error();
    const auto tmux_failed = invoke(
        {"load", "tf.yaml", "-d", "-S", fixture->socket_path().string(), "--json"});
    EXPECT_NE(tmux_failed.code, 0);
    const auto record = Json::parse(tmux_failed.err);
    EXPECT_EQ(record.at("schema_version"), 1);
    EXPECT_EQ(record.at("code"), "tmux_failed");
  }
  {
    auto fixture = libtmux::test::ScopedTmuxServer::start(
        {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-codes-sf")});
    ASSERT_TRUE(fixture.has_value()) << fixture.error();
    const auto script_failed = invoke(
        {"load", "sf.yaml", "-d", "-S", fixture->socket_path().string(), "--json"});
    EXPECT_NE(script_failed.code, 0);
    const auto record = Json::parse(script_failed.err);
    EXPECT_EQ(record.at("schema_version"), 1);
    EXPECT_EQ(record.at("code"), "script_failed");
  }
  {
    auto fixture = libtmux::test::ScopedTmuxServer::start(
        {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-codes-de")});
    ASSERT_TRUE(fixture.has_value()) << fixture.error();
    const auto socket = fixture->socket_path().string();
    ASSERT_EQ(invoke({"load", "ok.yaml", "-d", "-S", socket, "--json"}).code, 0);
    std::ofstream{"exists.yaml"} << "";
    const auto destination_exists = invoke(
        {"freeze", "ok", "-S", socket, "--json", "--save-to", "exists.yaml"});
    EXPECT_NE(destination_exists.code, 0);
    const auto record = Json::parse(destination_exists.err);
    EXPECT_EQ(record.at("schema_version"), 1);
    EXPECT_EQ(record.at("code"), "destination_exists");

    const auto usage = invoke({"load", "ok.yaml", "-S", socket, "--json"});
    EXPECT_EQ(usage.code, 2);
    const auto usage_record = Json::parse(usage.err);
    EXPECT_EQ(usage_record.at("schema_version"), 1);
    EXPECT_EQ(usage_record.at("code"), "usage");
  }
  {
    // A socket with no live server, and no tmux on PATH to start a fresh
    // one, takes the cold-start path rather than an ordinary command.
    auto fixture = libtmux::test::ScopedTmuxServer::start(
        {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-codes-tu")});
    ASSERT_TRUE(fixture.has_value()) << fixture.error();
    const auto cold_socket = (fixture->socket_path().parent_path() / "cold").string();
    const auto empty_path =
        std::filesystem::temp_directory_path() / "cxx-ws-empty-path";
    std::filesystem::create_directories(empty_path);
    libtmux::test::EnvironmentGuard path{"PATH", empty_path.string()};
    const auto tmux_unavailable =
        invoke({"load", "ok.yaml", "-d", "-S", cold_socket, "--json"});
    EXPECT_NE(tmux_unavailable.code, 0);
    const auto record = Json::parse(tmux_unavailable.err);
    EXPECT_EQ(record.at("schema_version"), 1);
    EXPECT_EQ(record.at("code"), "tmux_unavailable");
  }
}

// `<<: *anchor` or `<<: [*a, *b]` merges that mapping's keys, in order,
// with explicit keys overriding merged ones.
TEST(WorkspaceCli, ConvertResolvesYamlMergeKeys) {
  Files files;
  std::ofstream{"merge.yaml"}
      << "session_name: merge-test\nwindows:\n"
         "  - &base\n    window_name: a\n    panes: [echo a]\n"
         "  - <<: *base\n    window_name: b\n";
  const auto converted = invoke({"convert", "merge.yaml", "--json"});
  ASSERT_EQ(converted.code, 0) << converted.err;
  const auto document = Json::parse(converted.out);
  ASSERT_EQ(document.at("windows").size(), 2U);
  EXPECT_EQ(document.at("windows")[1].at("window_name"), "b");
  EXPECT_EQ(document.at("windows")[1].at("panes"), document.at("windows")[0].at("panes"));
  EXPECT_FALSE(document.at("windows")[1].contains("<<"));

  std::ofstream{"merge-list.yaml"}
      << "session_name: merge-list\nwindows:\n"
         "  - &one\n    window_name: one\n    panes: [echo one]\n"
         "  - &two\n    window_name: two\n    panes: [echo two]\n"
         "  - <<: [*one, *two]\n    panes: [echo three]\n";
  const auto listed = invoke({"convert", "merge-list.yaml", "--json"});
  ASSERT_EQ(listed.code, 0) << listed.err;
  const auto merged = Json::parse(listed.out).at("windows")[2];
  // The first merge source wins the collision on window_name; explicit
  // `panes` (present on the mapping itself) always wins over either.
  EXPECT_EQ(merged.at("window_name"), "one");
  EXPECT_EQ(merged.at("panes"), Json::array({"echo three"}));
}

TEST(WorkspaceCli, ConvertNamesTheDestinationAfterTheEncodingItWrites) {
  Files files;
  for (const std::string encoding : {"yaml", "json"}) {
    const std::string written = "source." + encoding;
    const std::string absent = encoding == "json" ? "source.yaml" : "source.json";
    std::ofstream{"source.yml"} << "session_name: converted\nwindows: [{}]\n";
    const auto result =
        invoke({"convert", "--yes", "--workspace-format", encoding, "source.yml"});
    ASSERT_EQ(result.code, 0) << encoding << result.err;
    EXPECT_FALSE(std::filesystem::exists(absent)) << encoding;
    ASSERT_TRUE(std::filesystem::exists(written)) << encoding;
    std::ifstream saved{written};
    const std::string bytes{std::istreambuf_iterator<char>{saved}, {}};
    EXPECT_EQ(Json::accept(bytes), encoding == "json") << bytes;
    std::filesystem::remove(written);
  }
}

// tmuxp has exactly one active global workspace directory (the first that
// exists of $TMUXP_CONFIGDIR, $XDG_CONFIG_HOME/tmuxp, then legacy ~/.tmuxp);
// a name that collides must resolve from the active one, and a name that
// exists only in an inactive directory must not be found.
TEST(WorkspaceCli, EditByNamePrefersActiveWorkspaceDirectoryOverLegacy) {
  Files files;
  ::unsetenv("TMUXP_CONFIGDIR");
  std::filesystem::create_directories("tmuxp");  // $XDG_CONFIG_HOME/tmuxp: active
  std::filesystem::create_directories(".tmuxp"); // legacy: inactive here
  std::ofstream{"tmuxp/dup.yaml"} << "session_name: dup-xdg\nwindows: [{}]\n";
  std::ofstream{".tmuxp/dup.yaml"} << "session_name: dup-legacy\nwindows: [{}]\n";
  std::ofstream{".tmuxp/alpha.yaml"} << "session_name: alpha-legacy\nwindows: [{}]\n";
  libtmux::test::EnvironmentGuard visual{"VISUAL", ""};
  libtmux::test::EnvironmentGuard editor{"EDITOR", "/bin/echo"};

  const auto found = invoke({"edit", "dup", "--json"});
  ASSERT_EQ(found.code, 0) << found.err;
  const auto path = Json::parse(found.out).at("stdout").get<std::string>();
  EXPECT_NE(path.find("/tmuxp/dup.yaml"), std::string::npos) << path;
  EXPECT_EQ(path.find("/.tmuxp/dup.yaml"), std::string::npos) << path;

  const auto missing = invoke({"edit", "alpha", "--json"});
  EXPECT_NE(missing.code, 0);
}

TEST(WorkspaceCli, EditorKeepsQuotedArgumentsAndChildStatus) {
  Files files;
  std::ofstream{"dev.yaml"} << "session_name: editor\nwindows: [{}]\n";
  std::ofstream{"editor script.sh"}
      << "printf '%s\\n' \"$1\" \"$2\"\nprintf 'child error' >&2\nexit 7\n";
  libtmux::test::EnvironmentGuard visual{"VISUAL", ""};
  libtmux::test::EnvironmentGuard editor{
      "EDITOR", "/bin/\\\nsh 'editor script.sh' \"a\\q\\$\\`x\\\ny\""};
  const auto result = invoke({"edit", "dev.yaml", "--json"});
  ASSERT_EQ(result.code, 7) << result.out << result.err;
  const auto document = Json::parse(result.out);
  EXPECT_EQ(document.at("exit_code"), 7);
  EXPECT_EQ(document.at("stdout"),
            "a\\q$`xy\n" + (files.directory / "dev.yaml").string() + "\n");
  EXPECT_EQ(document.at("stderr"), "child error");
  EXPECT_TRUE(result.err.empty());
  libtmux::test::EnvironmentGuard malformed{"EDITOR", "/bin/sh 'unfinished"};
  const auto refused = invoke({"edit", "dev.yaml", "--json"});
  EXPECT_EQ(refused.code, 2);
  EXPECT_TRUE(refused.out.empty());
}

TEST(WorkspaceCli, EditorLaunchFailureEndsItsNdjsonOperation) {
  Files files;
  std::ofstream{"dev.yaml"} << "session_name: editor\nwindows: [{}]\n";
  libtmux::test::EnvironmentGuard visual{"VISUAL", ""};
  libtmux::test::EnvironmentGuard editor{"EDITOR", "/missing-workspace-editor"};
  const auto result = invoke({"edit", "dev.yaml", "--ndjson"});
  ASSERT_EQ(result.code, 1);
  std::istringstream lines{result.out};
  std::string line;
  ASSERT_TRUE(static_cast<bool>(std::getline(lines, line)));
  EXPECT_EQ(Json::parse(line).at("event"), "started");
  ASSERT_TRUE(static_cast<bool>(std::getline(lines, line)));
  const auto failed = Json::parse(line);
  EXPECT_EQ(failed.at("event"), "failed");
  EXPECT_EQ(failed.at("sequence"), 2);
  EXPECT_FALSE(static_cast<bool>(std::getline(lines, line)));
  EXPECT_EQ(Json::parse(result.err).at("code"), "process_failed");
}

// A session built with no explicit size sits at tmux's `default-size`
// (80x24) until a client attaches, so every window is laid out wrong for the
// terminal the user is looking at, and only the window a client happens to
// focus first is ever corrected. `load` must size the session to the
// terminal up front, so every window it builds -- not only the focused
// one -- already has that size before any client exists.
TEST(WorkspaceCliTmux, LoadSizesEveryWindowToTheTerminalNotToDefaultSize) {
  Files files;
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-size")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());
  std::ofstream{"size.yaml"} << "session_name: sized\nwindows:\n"
                                "  - window_name: one\n    panes: [':']\n"
                                "  - window_name: two\n    panes: [':']\n";
  {
    // The test harness never gives `run()` a real std::cout, so terminal
    // detection is always off here; COLUMNS/LINES stand in for it.
    libtmux::test::EnvironmentGuard columns{"COLUMNS", "160"};
    libtmux::test::EnvironmentGuard lines{"LINES", "48"};
    const auto result = invoke(
        {"load", "size.yaml", "-d", "-S", fixture->socket_path().string(), "--json"});
    ASSERT_EQ(result.code, 0) << result.err;
  }
  const auto session = server->session("=sized:");
  ASSERT_TRUE(session.has_value());
  const auto windows = session->windows();
  ASSERT_TRUE(windows.has_value());
  ASSERT_EQ(windows->size(), 2U);
  for (const auto& window : *windows) {
    EXPECT_EQ(window.width(), 160) << window.name();
    EXPECT_EQ(window.height(), 48) << window.name();
  }
  ASSERT_TRUE(session->kill().has_value());

  // TMUXP_DETECT_TERMINAL_SIZE disabled: no -x/-y at all, tmux's own
  // default-size (80x24) governs, matching a script that wants that.
  libtmux::test::EnvironmentGuard disabled{"TMUXP_DETECT_TERMINAL_SIZE", "0"};
  libtmux::test::EnvironmentGuard columns{"COLUMNS", "160"};
  const auto result = invoke(
      {"load", "size.yaml", "-d", "-S", fixture->socket_path().string(), "--json"});
  ASSERT_EQ(result.code, 0) << result.err;
  const auto unsized = server->session("=sized:");
  ASSERT_TRUE(unsized.has_value());
  const auto unsized_windows = unsized->windows();
  ASSERT_TRUE(unsized_windows.has_value());
  EXPECT_EQ(unsized_windows->front().width(), 80);
  EXPECT_EQ(unsized_windows->front().height(), 24);
}

TEST(WorkspaceCliTmux, ImportedWorkspacesKeepCommandsFocusAndOptions) {
  Files files;
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-import")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());
  std::ofstream{"teamocil.yml"}
      << "name: team-native\nwindows:\n"
         "  - name: work\n    focus: true\n"
         "    options: {'@import-source': teamocil}\n"
         "    panes:\n      - commands: ':'\n        focus: true\n      - focus: true\n"
         "        commands: ['export IMPORT_SEQUENCE=teamocil', 'printf %s "
         "\"$IMPORT_SEQUENCE\" > teamocil-marker']\n"
         "  - name: other\n    focus: true\n    panes: [':']\n";
  std::ofstream{"tmuxinator.yml"}
      << "name: tmuxinator-native\nwindows:\n"
         "  - work: ['export IMPORT_SEQUENCE=tmuxinator', 'printf %s "
         "\"$IMPORT_SEQUENCE\" > tmuxinator-marker']\n";
  for (const auto* kind : {"teamocil", "tmuxinator"}) {
    const auto input = std::string{kind} + ".yml";
    const auto output = std::string{kind} + ".json";
    const auto imported =
        invoke({"import", kind, input, "--save-to", output, "--json"});
    ASSERT_EQ(imported.code, 0) << imported.out << imported.err;
    const auto loaded =
        invoke({"load", output, "-d", "-S", fixture->socket_path().string(), "--json"});
    ASSERT_EQ(loaded.code, 0) << loaded.out << loaded.err;
    const auto marker = std::string{kind} + "-marker";
    std::string contents;
    for (int wait = 0; wait < 300; ++wait) {
      std::ifstream observed{marker};
      contents.assign(std::istreambuf_iterator<char>{observed}, {});
      if (contents == kind)
        break;
      ::poll(nullptr, 0, 10);
    }
    EXPECT_EQ(contents, kind);
  }
  const auto team = server->session("=team-native:");
  ASSERT_TRUE(team.has_value());
  const auto active = team->active_window();
  ASSERT_TRUE(active.has_value());
  EXPECT_EQ(active->name(), "work");
  const auto origin = active->option("@import-source");
  ASSERT_TRUE(origin.has_value());
  EXPECT_EQ(origin->value, "teamocil");
  const auto panes = active->panes();
  ASSERT_TRUE(panes.has_value());
  ASSERT_EQ(panes->size(), 2U);
  EXPECT_TRUE((*panes)[0].active());
  const auto tmuxinator = server->session("=tmuxinator-native:");
  ASSERT_TRUE(tmuxinator.has_value());
  const auto window = tmuxinator->active_window();
  ASSERT_TRUE(window.has_value());
  const auto single = window->panes();
  ASSERT_TRUE(single.has_value());
  EXPECT_EQ(single->size(), 1U);
}

// Match tmuxp -- expand $VAR, ${VAR} and a leading ~ in command text from
// the *loading process's* environment before sending it, rather than
// leaving the literal text for the pane's own (differently-environed) shell
// to resolve later. QA_LOADER_ONLY is set only on this test process, never
// passed into the session/window `environment:`, so the pane's shell cannot
// resolve it itself -- only a loader-side expansion can produce it.
TEST(WorkspaceCliTmux, LoadExpandsShellVariablesFromTheLoadingProcessEnvironment) {
  Files files;
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-expand")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());
  libtmux::test::EnvironmentGuard loader{"QA_LOADER_ONLY", "fromloader"};
  std::ofstream{"expand.yaml"} << "session_name: expand-test\nwindows:\n"
                                  "  - panes:\n      - echo \"marker=$QA_LOADER_ONLY\"\n";
  const auto result = invoke(
      {"load", "expand.yaml", "-d", "-S", fixture->socket_path().string(), "--json"});
  ASSERT_EQ(result.code, 0) << result.err;
  const auto session = server->session("=expand-test:");
  ASSERT_TRUE(session.has_value());
  const auto window = session->active_window();
  ASSERT_TRUE(window.has_value());
  const auto panes = window->panes();
  ASSERT_TRUE(panes.has_value());
  ASSERT_FALSE(panes->empty());
  std::string captured;
  for (int wait = 0; wait < 300; ++wait) {
    const auto text = panes->front().capture();
    if (text.has_value() && text->find("marker=") != std::string::npos) {
      captured = *text;
      break;
    }
    ::poll(nullptr, 0, 10);
  }
  EXPECT_NE(captured.find("marker=fromloader"), std::string::npos) << captured;
  EXPECT_EQ(captured.find("$QA_LOADER_ONLY"), std::string::npos) << captured;
}

// A string scalar a YAML 1.1 (PyYAML/tmuxp) or 1.2 resolver would read back
// as bool, null or a number must stay quoted when cxx writes YAML, or the
// value comes back corrupted (a window named "yes" reloads as the boolean
// true, "1.0" as a float, ...).
TEST(WorkspaceCliTmux, ConvertToYamlQuotesScalarLookingWindowNames) {
  Files files;
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-yamlq")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());
  std::ofstream{"names.yaml"}
      << "session_name: names-test\nwindows:\n"
         "  - window_name: \"yes\"\n    panes: [':']\n"
         "  - window_name: \"1.0\"\n    panes: [':']\n"
         "  - window_name: \"08\"\n    panes: [':']\n"
         "  - window_name: \"off\"\n    panes: [':']\n";
  const auto converted = invoke({"convert", "names.yaml", "--json"});
  ASSERT_EQ(converted.code, 0) << converted.err;
  const auto saved = invoke({"convert", "--save-to", "roundtrip.yaml",
                             "--workspace-format", "yaml", "--yes", "--force",
                             "names.yaml"});
  ASSERT_EQ(saved.code, 0) << saved.err;
  std::ifstream written{"roundtrip.yaml"};
  const std::string text{std::istreambuf_iterator<char>{written}, {}};
  for (const auto* quoted : {"\"yes\"", "\"1.0\"", "\"08\"", "\"off\""})
    EXPECT_NE(text.find(quoted), std::string::npos) << text;

  const auto loaded = invoke({"load", "roundtrip.yaml", "-d", "-S",
                              fixture->socket_path().string(), "--json"});
  ASSERT_EQ(loaded.code, 0) << loaded.err << text;
  const auto session = server->session("=names-test:");
  ASSERT_TRUE(session.has_value());
  const auto windows = session->windows();
  ASSERT_TRUE(windows.has_value());
  ASSERT_EQ(windows->size(), 4U);
  const std::vector<std::string> expected{"yes", "1.0", "08", "off"};
  for (std::size_t index = 0; index < expected.size(); ++index)
    EXPECT_EQ(std::string{(*windows)[index].name()}, expected[index]);
}

TEST(WorkspaceCliTmux, NativeLoadCaptureAndConversionRoundTrip) {
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());
  ASSERT_TRUE(server->new_session("cli-created-longer").has_value());
  const auto absent = invoke(
      {"freeze", "cli-created", "-S", fixture->socket_path().string(), "--json"});
  EXPECT_EQ(absent.code, 1);
  EXPECT_TRUE(absent.out.empty());
  const auto file = fixture->socket_path().parent_path() / "workspace.yaml";
  std::ofstream{file} << "session_name: cli-created\nwindows:\n  - window_name: one\n"
                         "    window_index: 4\n    panes: [null, null]\n";
  const auto loaded = invoke(
      {"--json", "load", file.string(), "-d", "-S", fixture->socket_path().string()});
  ASSERT_EQ(loaded.code, 0) << loaded.err;
  ASSERT_FALSE(loaded.out.empty());
  const auto summary = Json::parse(loaded.out);
  EXPECT_EQ(summary.at("status"), "ok");
  EXPECT_EQ(summary.at("results").size(), 1U);
  const auto captured = invoke(
      {"freeze", "cli-created", "-S", fixture->socket_path().string(), "--json"});
  ASSERT_EQ(captured.code, 0) << captured.err;
  const auto document = Json::parse(captured.out);
  EXPECT_EQ(document.at("session_name"), "cli-created");
  EXPECT_EQ(document.at("windows")[0].at("window_index"), 4);
  EXPECT_EQ(document.at("windows")[0].at("panes").size(), 2U);
  ASSERT_FALSE(captured.err.empty());
  EXPECT_EQ(Json::parse(captured.err).at("code"), "capture_lossy");
  const auto converted = invoke({"convert", file.string(), "--json"});
  ASSERT_EQ(converted.code, 0) << converted.err;
  EXPECT_EQ(Json::parse(converted.out).at("session_name"), "cli-created");
  const auto reused = invoke(
      {"load", file.string(), "-d", "-S", fixture->socket_path().string(), "--ndjson"});
  ASSERT_EQ(reused.code, 0) << reused.err;
  std::istringstream lines{reused.out};
  std::string line;
  std::size_t sequence = 0;
  int completed = 0;
  while (std::getline(lines, line)) {
    const auto event = Json::parse(line);
    EXPECT_EQ(event.at("sequence"), ++sequence);
    completed += event.at("event") == "completed" ? 1 : 0;
  }
  EXPECT_EQ(completed, 1);
  EXPECT_GT(sequence, 2U);
}

TEST(WorkspaceCliTmux, CaptureReloadsExplicitLocalOptions) {
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("capture")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());
  ASSERT_TRUE(server->set_global_option("@inherited", "global only").has_value());
  const auto source = server->new_session("capture-original");
  ASSERT_TRUE(source.has_value());
  const auto window = source->active_window();
  ASSERT_TRUE(window.has_value());
  const std::string value = "quotes ' \" \\ and = equals\na second line\twith tabs";
  ASSERT_TRUE(source->set_option("status", "off").has_value());
  ASSERT_TRUE(source->set_option("@literal*", value).has_value());
  ASSERT_TRUE(source->set_option("status-format[3]", value).has_value());
  ASSERT_TRUE(window->set_option("automatic-rename", "off").has_value());
  ASSERT_TRUE(window->set_option("@window-note", value).has_value());
  ASSERT_TRUE(window->set_option("synchronize-panes", "on").has_value());
  const auto decoy = server->new_session("capture-original-longer");
  ASSERT_TRUE(decoy.has_value());
  ASSERT_TRUE(decoy->set_option("status", "on").has_value());

  const auto captured = invoke(
      {"freeze", "capture-original", "-S", fixture->socket_path().string(), "--json"});
  ASSERT_EQ(captured.code, 0) << captured.err;
  auto document = Json::parse(captured.out);
  const auto options = document.value("options", Json::object());
  EXPECT_EQ(options.value("status", "missing"), "off");
  EXPECT_EQ(options.value("@literal*", "missing"), value);
  EXPECT_EQ(options.value("status-format[3]", "missing"), value);
  EXPECT_FALSE(options.contains("@inherited"));
  EXPECT_FALSE(document.contains("global_options"));
  // Window options round-trip through `options_after`: `automatic-rename:
  // off` only holds if it is applied once the window's panes already exist.
  EXPECT_FALSE(document.at("windows")[0].contains("options"));
  const auto window_options =
      document.at("windows")[0].value("options_after", Json::object());
  EXPECT_EQ(window_options.value("automatic-rename", "missing"), "off");
  EXPECT_EQ(window_options.value("@window-note", "missing"), value);
  EXPECT_EQ(window_options.value("synchronize-panes", "missing"), "on");
  const auto by_id = invoke({"freeze", std::string{source->id()}, "-S",
                             fixture->socket_path().string(), "--json"});
  ASSERT_EQ(by_id.code, 0) << by_id.err;
  EXPECT_EQ(Json::parse(by_id.out), document);

  document["session_name"] = "capture-restored";
  const auto file = fixture->socket_path().parent_path() / "captured.json";
  std::ofstream{file} << document.dump();
  const auto loaded = invoke(
      {"load", file.string(), "-d", "-S", fixture->socket_path().string(), "--json"});
  ASSERT_EQ(loaded.code, 0) << loaded.err << loaded.out;
  const auto restored = server->session("=capture-restored:");
  ASSERT_TRUE(restored.has_value());
  const auto restored_window = restored->active_window();
  ASSERT_TRUE(restored_window.has_value());
  const auto status = restored->option("status");
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(status->value, "off");
  EXPECT_FALSE(status->inherited);
  for (const auto* name : {"@literal*", "status-format[3]"}) {
    const auto option = restored->option(name);
    ASSERT_TRUE(option.has_value()) << name;
    EXPECT_EQ(option->value, value);
    EXPECT_FALSE(option->inherited);
  }
  const auto note = restored_window->option("@window-note");
  ASSERT_TRUE(note.has_value());
  EXPECT_EQ(note->value, value);
  const auto synchronized = restored_window->option("synchronize-panes");
  ASSERT_TRUE(synchronized.has_value());
  EXPECT_EQ(synchronized->value, "on");
  EXPECT_TRUE(fixture->is_alive());
}

TEST(WorkspaceCliTmux, CaptureLeavesOutTheShellTmuxStartedForThePane) {
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("capshl")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());
  const auto session = server->new_session("capture-shell");
  ASSERT_TRUE(session.has_value());
  const auto window = session->active_window();
  ASSERT_TRUE(window.has_value());
  const auto panes = window->panes();
  ASSERT_TRUE(panes.has_value());
  // How tmux names the shell it started is its business and differs by
  // platform, so the option is pointed at that name rather than the reverse.
  const std::string running{panes->front().command()};
  ASSERT_FALSE(running.empty());
  const auto directory = fixture->socket_path().parent_path();
  for (const auto& name : {running, std::string{"not-a-shell"}})
    std::filesystem::create_symlink("/bin/sh", directory / name);
  const auto freeze = [&] {
    const auto result = invoke(
        {"freeze", "capture-shell", "-S", fixture->socket_path().string(), "--json"});
    EXPECT_EQ(result.code, 0) << result.err;
    return Json::parse(result.out).at("windows")[0].at("panes")[0];
  };
  // Reloading a named default shell builds a shell inside a shell, so the
  // key is left out rather than emitted as an empty command list.
  ASSERT_TRUE(server->set_global_option("default-shell", (directory / running).string())
                  .has_value());
  EXPECT_FALSE(freeze().contains("shell_command"));

  // The same pane again, once tmux would start something else for it: a
  // command that is not the session's shell is one a reload has to run.
  ASSERT_TRUE(
      server->set_global_option("default-shell", (directory / "not-a-shell").string())
          .has_value());
  EXPECT_EQ(freeze().at("shell_command"), Json::array({running}));
}

TEST(WorkspaceCliTmux, CaptureTreatsAnyOrdinaryShellAsTheDefaultOne) {
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("capsh2")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());
  // default-command reproduces macOS's /bin/sh-is-bash mismatch on Linux.
  ASSERT_TRUE(server->set_global_option("default-shell", "/bin/sh").has_value());
  ASSERT_TRUE(server->set_global_option("default-command", "/bin/bash -i").has_value());
  const auto session = server->new_session("capture-mismatch");
  ASSERT_TRUE(session.has_value());
  const auto window = session->active_window();
  ASSERT_TRUE(window.has_value());
  const auto panes = window->panes();
  ASSERT_TRUE(panes.has_value());
  std::string observed;
  for (int attempt = 0; attempt < 200; ++attempt) {
    observed = panes->front().command();
    if (observed == "bash")
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  ASSERT_EQ(observed, "bash") << "fixture did not settle on bash";
  const auto result = invoke(
      {"freeze", "capture-mismatch", "-S", fixture->socket_path().string(), "--json"});
  ASSERT_EQ(result.code, 0) << result.err;
  const auto pane = Json::parse(result.out).at("windows")[0].at("panes")[0];
  EXPECT_FALSE(pane.contains("shell_command")) << pane;
}

TEST(WorkspaceCliTmux, FreezeSavesBlockStyleYaml) {
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("capblk")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());
  const auto session = server->new_session("capture-block");
  ASSERT_TRUE(session.has_value());
  ASSERT_TRUE(session->set_option("status", "off").has_value());
  const auto window = session->active_window();
  ASSERT_TRUE(window.has_value());
  ASSERT_TRUE(window->split().has_value());

  const auto destination = fixture->socket_path().parent_path() / "frozen.yaml";
  const auto result =
      invoke({"freeze", "capture-block", "-S", fixture->socket_path().string(),
              "--save-to", destination.string()});
  ASSERT_EQ(result.code, 0) << result.err;
  ASSERT_TRUE(std::filesystem::exists(destination));
  std::ifstream saved{destination};
  const std::string bytes{std::istreambuf_iterator<char>{saved}, {}};
  // Block style, not one JSON-shaped line.
  EXPECT_NE(bytes.find("session_name: capture-block\n"), std::string::npos) << bytes;
  EXPECT_NE(bytes.find("options:\n"), std::string::npos) << bytes;
  EXPECT_NE(bytes.find("windows:\n"), std::string::npos) << bytes;
  EXPECT_NE(bytes.find("  - window_name:"), std::string::npos) << bytes;
  EXPECT_NE(bytes.find("\n    panes:\n"), std::string::npos) << bytes;
  EXPECT_NE(bytes.find("\n      - start_directory:"), std::string::npos) << bytes;
  EXPECT_GT(std::count(bytes.begin(), bytes.end(), '\n'), 8) << bytes;
}

TEST(WorkspaceCliTmux, CaptureOptionReadFailureDoesNotPublish) {
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("capfail")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto shim = fixture->socket_path().parent_path() / "shim";
  std::filesystem::create_directory(shim);
  const auto executable = shim / "tmux";
  std::ofstream{executable}
      << "#!/bin/sh\nexport PATH=\"$CXX_ORIGINAL_PATH\"\n"
         "show=no\nwindow=no\nfor argument do\n"
         "[ \"$argument\" = show-options ] && show=yes\n"
         "[ \"$argument\" = -w ] && window=yes\ndone\n"
         "if [ \"$show\" = yes ] && [ \"$window\" = \"$CXX_CAPTURE_WINDOW\" ]; then\n"
         "printf 'capture option read refused\\n' >&2\nexit 1\nfi\n"
         "exec tmux \"$@\"\n";
  std::filesystem::permissions(executable, std::filesystem::perms::owner_all);
  const auto destination = fixture->socket_path().parent_path() / "capture.json";
  const std::string previous_path = std::getenv("PATH");
  const libtmux::test::EnvironmentGuard original{"CXX_ORIGINAL_PATH", previous_path};
  const libtmux::test::EnvironmentGuard path{"PATH",
                                             shim.string() + ":" + previous_path};
  for (const auto* scope : {"no", "yes"}) {
    SCOPED_TRACE(scope);
    const libtmux::test::EnvironmentGuard window_scope{"CXX_CAPTURE_WINDOW", scope};
    const auto captured = invoke({"freeze", std::string{fixture->session_name()}, "-S",
                                  fixture->socket_path().string(), "--json",
                                  "--save-to", destination.string()});
    EXPECT_EQ(captured.code, 1);
    EXPECT_TRUE(captured.out.empty());
    EXPECT_FALSE(std::filesystem::exists(destination));
    ASSERT_FALSE(captured.err.empty());
    const auto error = Json::parse(captured.err);
    EXPECT_EQ(error.at("code"), "capture_failed");
    EXPECT_NE(
        error.at("message").get<std::string>().find("capture option read refused"),
        std::string::npos);
  }
}

TEST(WorkspaceCliTmux, AppendKeepsItsBorrowedSessionAndReportsRetainedWindows) {
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-append")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());
  const auto session = server->new_session("borrowed");
  ASSERT_TRUE(session.has_value());
  const auto pane = session->active_pane();
  ASSERT_TRUE(pane.has_value());
  const auto original_window = session->active_window();
  ASSERT_TRUE(original_window.has_value());
  const auto pid = pane->expand("#{pid}");
  ASSERT_TRUE(pid.has_value());
  libtmux::test::EnvironmentGuard tmux{"TMUX", fixture->socket_path().string() + "," +
                                                   *pid + ",0"};
  libtmux::test::EnvironmentGuard current_pane{"TMUX_PANE", pane->id()};
  const auto file = fixture->socket_path().parent_path() / "append.yaml";
  std::ofstream{file}
      << "session_name: ignored-name\nenvironment: {WS_APPEND: 'yes'}\n"
         "windows: [{window_name: appended, window_index: 9, panes: [null, null]}]\n";
  const auto result = invoke({"load", file.string(), "--append", "--json"});
  ASSERT_EQ(result.code, 0) << result.err << result.out;
  const auto summary = Json::parse(result.out);
  ASSERT_EQ(summary.at("results").size(), 1U);
  EXPECT_EQ(summary.at("results")[0].at("action"), "appended");
  EXPECT_EQ(summary.at("results")[0].at("session_name"), "borrowed");
  EXPECT_FALSE(server->session("=ignored-name:").has_value());
  auto windows = session->windows();
  ASSERT_TRUE(windows.has_value());
  ASSERT_EQ(windows->size(), 2U);
  EXPECT_TRUE(original_window->panes().has_value());
  const auto appended = server->window(std::string{session->id()} + ":9");
  ASSERT_TRUE(appended.has_value());
  const auto panes = appended->panes();
  ASSERT_TRUE(panes.has_value());
  EXPECT_EQ(panes->size(), 2U);
  const auto environment =
      server->run({"show-environment", "-t", std::string{session->id()}, "WS_APPEND"});
  ASSERT_TRUE(environment.has_value());
  EXPECT_EQ(*environment, "WS_APPEND=yes\n");

  // Invalid layouts crash tmux 3.3a; an unknown option returns a command error.
  std::ofstream{file} << "session_name: ignored-name\n"
                         "windows: [{options: {not-a-window-option: 'on'}}]\n";
  const auto failed = invoke({"load", file.string(), "--append", "--json"});
  ASSERT_EQ(failed.code, 1) << failed.err << failed.out;
  const auto partial = Json::parse(failed.out);
  EXPECT_EQ(partial.at("status"), "partial");
  const auto retained = partial.at("errors")[0].at("retained_state");
  EXPECT_EQ(retained.at("session_id"), session->id());
  ASSERT_EQ(retained.at("window_ids").size(), 1U);
  EXPECT_TRUE(server
                  ->window(std::string{session->id()} + ":" +
                           retained.at("window_ids")[0].get<std::string>())
                  .has_value());
  EXPECT_TRUE(original_window->panes().has_value());
  windows = session->windows();
  ASSERT_TRUE(windows.has_value());
  EXPECT_EQ(windows->size(), 3U);

  const auto cold = fixture->socket_path().parent_path() / "foreign.sock";
  const auto refused =
      invoke({"load", file.string(), "--append", "-S", cold.string(), "--json"});
  EXPECT_EQ(refused.code, 1);
  EXPECT_FALSE(std::filesystem::exists(cold));
  EXPECT_TRUE(original_window->panes().has_value());
  {
    libtmux::test::EnvironmentGuard stale{"TMUX",
                                          fixture->socket_path().string() + ",0,0"};
    std::ofstream{file} << "session_name: ignored-name\nwindows: [{}]\n";
    const auto rejected = invoke({"load", file.string(), "--append", "--json"});
    EXPECT_EQ(rejected.code, 1);
    const auto unchanged = session->windows();
    ASSERT_TRUE(unchanged.has_value());
    EXPECT_EQ(unchanged->size(), 3U);
  }
  libtmux::test::EnvironmentGuard outside{"TMUX", ""};
  const auto missing = invoke({"load", file.string(), "--append", "--json"});
  EXPECT_EQ(missing.code, 2);
  EXPECT_TRUE(missing.out.empty());
}

// Human output is for humans. A cold-start failure retains an
// unverified bootstrap session as machine data (--json's errors[0] carries
// it as retained_state); human mode must not print that record as raw JSON.
// This exercises execute_impl()'s own per-error diagnostic (already
// correct); cli.cpp's separate top-level `fail()` closure had the same
// "Retained state: " + encoded(...) bug for a load whose build succeeds but
// whose interactive attach then fails. That path needs a live controlling
// terminal, which this in-process suite cannot give it (validate()
// otherwise refuses with "usage") -- tools/verify_process.py's
// terminal_switch() covers it instead, via a real pty.
TEST(WorkspaceCliTmux, HumanErrorNeverPrintsRetainedStateAsJson) {
  Files files;
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-humanerr")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  std::ofstream{"ok.yaml"} << "session_name: ok\nwindows: [{panes: [echo]}]\n";
  const auto cold_socket = (fixture->socket_path().parent_path() / "cold-human").string();
  const auto empty_path =
      std::filesystem::temp_directory_path() / "cxx-ws-empty-path-human";
  std::filesystem::create_directories(empty_path);
  libtmux::test::EnvironmentGuard path{"PATH", empty_path.string()};
  const auto failed = invoke({"load", "ok.yaml", "-d", "-S", cold_socket});
  ASSERT_NE(failed.code, 0);
  EXPECT_EQ(failed.err.find("Retained state:"), std::string::npos) << failed.err;
  EXPECT_EQ(failed.err.find("\"schema_version\""), std::string::npos) << failed.err;
  // One "Error: " prefix -- not the diagnostic's own, and then a second one
  // introducing a "Retained state:" dump.
  EXPECT_EQ(std::count(failed.err.begin(), failed.err.end(), '\n'), 1) << failed.err;
  EXPECT_NE(failed.err.find("Error: "), std::string::npos) << failed.err;
}

TEST(WorkspaceCliTmux, ColdLoadRetainsTheWorkspaceAndRemovesItsBootstrap) {
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-cold")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto socket = fixture->socket_path().parent_path() / "cold.sock";
  struct Cleanup {
    std::filesystem::path socket;
    ~Cleanup() {
      const auto server = libtmux::Server::at_socket_path(socket.string());
      if (server)
        (void)server->kill();
    }
  } cleanup{socket};
  const auto file = socket.parent_path() / "cold.yaml";
  std::ofstream{file} << "session_name: invalid:name\nwindows: [{}]\n";
  const auto invalid =
      invoke({"load", file.string(), "-d", "-S", socket.string(), "--json"});
  EXPECT_EQ(invalid.code, 1);
  EXPECT_TRUE(invalid.out.empty());
  EXPECT_FALSE(std::filesystem::exists(socket));
  const auto configuration = socket.parent_path() / "tmux.conf";
  std::ofstream{configuration} << "set-option -g base-index 7\n";
  std::ofstream{file} << "session_name: cold\nwindows: [{}]\n";
  const auto loaded = invoke({"load", file.string(), "-d", "-S", socket.string(), "-f",
                              configuration.string(), "-2", "--ndjson"});
  ASSERT_EQ(loaded.code, 0) << loaded.err;
  const auto server = libtmux::Server::at_socket_path(socket.string());
  ASSERT_TRUE(server.has_value());
  const auto sessions = server->sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  ASSERT_EQ(sessions->size(), 1U);
  EXPECT_EQ(sessions->front().name(), "cold");
  const auto windows = sessions->front().windows();
  ASSERT_TRUE(windows.has_value());
  ASSERT_EQ(windows->size(), 1U);
  EXPECT_EQ(windows->front().index(), 7);
  EXPECT_TRUE(fixture->is_alive());
  ASSERT_TRUE(server->kill().has_value());

  std::ofstream{file} << "session_name: cold-failed\n"
                         "windows: [{options: {not-a-window-option: 'on'}}]\n";
  const auto failed =
      invoke({"load", file.string(), "-d", "-S", socket.string(), "--json"});
  EXPECT_EQ(failed.code, 1);
  const auto reopened = libtmux::Server::at_socket_path(socket.string());
  ASSERT_TRUE(reopened.has_value());
  const auto after = reopened->sessions();
  EXPECT_TRUE(!after || after->empty());
  EXPECT_TRUE(fixture->is_alive());
  const auto missing = socket.parent_path() / "missing-parent" / "socket";
  const auto startup_failure =
      invoke({"load", file.string(), "-d", "-S", missing.string(), "--ndjson"});
  EXPECT_EQ(startup_failure.code, 1);
  std::istringstream events{startup_failure.out};
  std::string line;
  Json terminal;
  while (std::getline(events, line))
    terminal = Json::parse(line);
  EXPECT_EQ(terminal.at("event"), "failed");

  const auto shim = socket.parent_path() / "shim";
  std::filesystem::create_directory(shim);
  const auto executable = shim / "tmux";
  std::ofstream{executable}
      << "#!/bin/sh\nexport PATH=\"$CXX_ORIGINAL_PATH\"\n"
         "for argument do\nif [ \"$argument\" = new-session ]; then\n"
         "tmux \"$@\" >/dev/null\ncode=$?\nprintf 'invalid identity\\n'\n"
         "exit \"$code\"\nfi\ndone\nexec tmux \"$@\"\n";
  std::filesystem::permissions(executable, std::filesystem::perms::owner_all);
  Result malformed;
  {
    const std::string previous_path = std::getenv("PATH");
    const libtmux::test::EnvironmentGuard original{"CXX_ORIGINAL_PATH", previous_path};
    const libtmux::test::EnvironmentGuard path{"PATH",
                                               shim.string() + ":" + previous_path};
    malformed = invoke({"load", file.string(), "-d", "-S", socket.string(), "--json"});
  }
  EXPECT_EQ(malformed.code, 1);
  const auto problem = Json::parse(malformed.out).at("errors")[0];
  ASSERT_TRUE(problem.contains("retained_state"));
  EXPECT_EQ(problem.at("retained_state").at("ownership"), "unverified");
  const auto uncertain = libtmux::Server::at_socket_path(socket.string());
  ASSERT_TRUE(uncertain.has_value());
  const auto retained = uncertain->sessions();
  ASSERT_TRUE(retained.has_value());
  ASSERT_EQ(retained->size(), 1U);
  EXPECT_EQ(retained->front().name(),
            problem.at("retained_state").at("session_name").get<std::string>());
}

TEST(WorkspaceCliTmux, FailedEventDeliveryRetainsCompletedSessionAccounting) {
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-events")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());
  for (const bool closed : {false, true}) {
    const std::string name = closed ? "retained-closed" : "retained-runtime";
    const auto previous = server->sessions();
    ASSERT_TRUE(previous.has_value());
    const auto file = fixture->socket_path().parent_path() / "events.yaml";
    std::ofstream{file} << "session_name: " << name << "\nwindows: [{}]\n";
    const libtmux::workspace::cli::Request request{
        .command = "load",
        .importer = {},
        .values = {{"workspace-file", {file.string()}},
                   {"d", {"true"}},
                   {"S", {fixture->socket_path().string()}}},
        .ndjson = true};
    Json result;
    bool rejected{};
    try {
      result = libtmux::workspace::cli::execute(
                   request,
                   [&](const std::string& event, const libtmux::workspace::cli::Json&) {
                     if (event == "session-created") {
                       rejected = true;
                       if (closed)
                         throw libtmux::workspace::cli::Failure{1, "output_closed",
                                                                "event sink closed"};
                       throw std::runtime_error{"event sink failed"};
                     }
                   })
                   .value;
    } catch (const libtmux::workspace::cli::Failure& error) {
      EXPECT_EQ(error.exit_code, 1);
      result = error.retained_state;
    }
    EXPECT_TRUE(rejected);
    const auto retained = server->session("=" + name + ":");
    ASSERT_TRUE(retained.has_value());
    const auto sessions = server->sessions();
    ASSERT_TRUE(sessions.has_value());
    EXPECT_EQ(sessions->size(), previous->size() + 1);
    ASSERT_TRUE(result.is_object()) << "completed-session accounting was lost";
    EXPECT_EQ(result.at("status"), "partial");
    EXPECT_EQ(result.at("exit_code"), 1);
    ASSERT_EQ(result.at("results").size(), 1U);
    EXPECT_EQ(result.at("results")[0].at("session_id"), retained->id());
    EXPECT_EQ(result.at("errors")[0].at("code"),
              closed ? "output_closed" : "operation_failed");
  }
}

TEST(WorkspaceCliTmux, FailedScriptEventsRetainEffectsAndKnownStatus) {
  for (const bool appending : {false, true}) {
    for (const std::string rejected_event : {"script-output", "script-completed"}) {
      SCOPED_TRACE(rejected_event + (appending ? " append" : " create"));
      Files files;
      auto fixture = libtmux::test::ScopedTmuxServer::start(
          {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-events")});
      ASSERT_TRUE(fixture.has_value()) << fixture.error();
      const auto server =
          libtmux::Server::at_socket_path(fixture->socket_path().string());
      ASSERT_TRUE(server.has_value());
      const auto borrowed = server->session(fixture->session_name());
      ASSERT_TRUE(borrowed.has_value());
      const auto pane = borrowed->active_pane();
      ASSERT_TRUE(pane.has_value());
      const auto daemon = pane->expand("#{pid}");
      ASSERT_TRUE(daemon.has_value());
      libtmux::test::EnvironmentGuard tmux{"TMUX", fixture->socket_path().string() +
                                                       "," + *daemon + ",0"};
      libtmux::test::EnvironmentGuard current_pane{"TMUX_PANE", pane->id()};
      std::ofstream{"first.yaml"} << "session_name: completed\nwindows: [{}]\n";
      std::ofstream{"last.yaml"} << "session_name: later\nwindows: [{}]\n";
      std::ofstream{"before.sh"}
          << "echo $$ >script.pid\n"
             "tmux -S \"$1\" new-window -d -P -F '#{window_id}' -t \"$2\" "
             "-n from-script >script.window || exit 9\n"
             "printf ready\n"
          << (rejected_event == "script-completed" ? "kill -TERM $$\n"
                                                   : "exec sleep 30\n");
      std::ofstream{"second.json"}
          << Json{{"session_name", "failing"},
                  {"windows", Json::array({Json::object()})},
                  {"before_script",
                   "/bin/sh before.sh '" + fixture->socket_path().string() + "' '" +
                       (appending ? std::string{borrowed->id()} : "=failing:") + "'"}};
      libtmux::workspace::cli::Request request{
          .command = "load",
          .importer = {},
          .values = {{"workspace-file", {"first.yaml", "second.json", "last.yaml"}},
                     {appending ? "append" : "d", {"true"}},
                     {"S", {fixture->socket_path().string()}}},
          .ndjson = true};
      bool rejected{}, later_started{};
      Json result;
      try {
        result = libtmux::workspace::cli::execute(
                     request,
                     [&](const std::string& event,
                         const libtmux::workspace::cli::Json& value) {
                       if (event == "workspace-started" && value.at("input_index") == 2)
                         later_started = true;
                       if (event == rejected_event) {
                         rejected = true;
                         if (event == "script-completed") {
                           EXPECT_EQ(value.at("script_output").at("exit_code"), 143);
                         }
                         throw libtmux::workspace::cli::Failure{
                             1, "output_closed", "script event sink closed"};
                       }
                     })
                     .value;
      } catch (const libtmux::workspace::cli::Failure& error) {
        result = error.retained_state;
      }
      EXPECT_TRUE(rejected);
      EXPECT_FALSE(later_started);
      const auto retained = server->session(appending ? borrowed->id() : "=completed:");
      ASSERT_TRUE(retained.has_value());
      EXPECT_FALSE(server->session("=failing:").has_value());
      EXPECT_FALSE(server->session("=later:").has_value());
      const auto sessions = server->sessions();
      ASSERT_TRUE(sessions.has_value());
      EXPECT_EQ(sessions->size(), appending ? 1U : 2U);
      std::string script_window;
      std::ifstream{"script.window"} >> script_window;
      ASSERT_FALSE(script_window.empty());
      EXPECT_EQ(server->window(script_window).has_value(), appending);
      const auto windows = borrowed->windows();
      ASSERT_TRUE(windows.has_value());
      EXPECT_EQ(windows->size(), appending ? 3U : 1U);
      int child{};
      std::ifstream{"script.pid"} >> child;
      ASSERT_GT(child, 0);
      errno = 0;
      EXPECT_EQ(::kill(child, 0), -1);
      EXPECT_EQ(errno, ESRCH);
      EXPECT_TRUE(result.is_object()) << "script-event accounting was lost";
      if (!result.is_object())
        continue;
      EXPECT_EQ(result.at("status"), "partial");
      EXPECT_EQ(result.at("exit_code"), rejected_event == "script-completed" ? 143 : 1);
      ASSERT_EQ(result.at("results").size(), 1U);
      EXPECT_EQ(result.at("results")[0].at("session_id"), retained->id());
      const auto& problem = result.at("errors")[0];
      if (rejected_event == "script-completed") {
        EXPECT_TRUE(problem.contains("script_output"));
        if (problem.contains("script_output")) {
          EXPECT_EQ(problem.at("script_output").at("exit_code"), 143);
          EXPECT_EQ(problem.at("script_output").at("stdout"), "ready");
        }
      }
      if (appending) {
        EXPECT_EQ(problem.at("retained_state").at("session_id"), borrowed->id());
        EXPECT_TRUE(problem.at("retained_state").at("window_ids").empty());
      }
    }
  }
}

TEST(WorkspaceCliTmux, FailedPublicationRetainsSessionsAndPrimaryStatus) {
  Files files;
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-output")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());
  struct FailedFlush : std::stringbuf {
    int sync() override { return -1; }
  };
  for (const bool diagnostic_failure : {false, true}) {
    const std::string name = diagnostic_failure ? "error-closed" : "output-closed";
    std::ofstream{"output.yaml"} << "session_name: " << name << "\nwindows: [{}]\n";
    FailedFlush buffer;
    std::ostream broken{&buffer};
    broken.exceptions(std::ios::badbit);
    std::ostringstream working;
    std::istringstream input;
    EXPECT_EQ(
        libtmux::workspace::cli::run({"--log-level", "info", "load", "--log-file",
                                      "publication.log", "output.yaml", "-d", "-S",
                                      fixture->socket_path().string(), "--json"},
                                     input, diagnostic_failure ? working : broken,
                                     diagnostic_failure ? broken : working),
        1);
    ASSERT_TRUE(server->session("=" + name + ":").has_value());
    const auto published = Json::parse(working.str());
    if (diagnostic_failure)
      EXPECT_EQ(published.at("results")[0].at("session_name"), name);
    else
      EXPECT_EQ(published.at("retained_state").at("results")[0].at("session_name"),
                name);
  }
  const auto borrowed = server->session("=error-closed:");
  ASSERT_TRUE(borrowed.has_value());
  const auto pane = borrowed->active_pane();
  ASSERT_TRUE(pane.has_value());
  const auto pid = pane->expand("#{pid}");
  ASSERT_TRUE(pid.has_value());
  libtmux::test::EnvironmentGuard tmux{"TMUX", fixture->socket_path().string() + "," +
                                                   *pid + ",0"};
  libtmux::test::EnvironmentGuard current_pane{"TMUX_PANE", pane->id()};
  std::ofstream{"output.yaml"}
      << "session_name: append\nbefore_script: /bin/false\nwindows: [{}]\n";
  FailedFlush buffer;
  std::ostream broken{&buffer};
  std::istringstream input;
  std::ostringstream diagnostic;
  EXPECT_EQ(libtmux::workspace::cli::run({"--log-level", "info", "load", "--log-file",
                                          "publication.log", "output.yaml", "--append",
                                          "--json"},
                                         input, broken, diagnostic),
            1);
  std::istringstream lines{diagnostic.str()};
  std::string line;
  Json last;
  while (std::getline(lines, line))
    last = Json::parse(line);
  const auto state = last.at("retained_state");
  EXPECT_EQ(state.at("status"), "partial");
  EXPECT_TRUE(state.at("results").empty());
  EXPECT_EQ(state.at("errors")[0].at("retained_state").at("session_id"),
            borrowed->id());
  EXPECT_TRUE(server->session(borrowed->id()).has_value());
  std::ofstream{"output.yaml"}
      << "session_name: interrupted\nbefore_script: /bin/sh -c 'kill -TERM $$'\n"
         "windows: [{}]\n";
  for (const bool throwing : {false, true}) {
    FailedFlush interrupted_buffer;
    std::ostream interrupted_output{&interrupted_buffer};
    if (throwing)
      interrupted_output.exceptions(std::ios::badbit);
    std::ostringstream interrupted_error;
    EXPECT_EQ(
        libtmux::workspace::cli::run({"--log-level", "info", "load", "--log-file",
                                      "publication.log", "output.yaml", "-d", "-S",
                                      fixture->socket_path().string(), "--json"},
                                     input, interrupted_output, interrupted_error),
        143);
    EXPECT_FALSE(server->session("=interrupted:").has_value());
  }
  std::ifstream log{"publication.log"};
  ASSERT_TRUE(log.is_open());
  bool publication_failure{};
  while (std::getline(log, line)) {
    const auto record = Json::parse(line);
    publication_failure |= record.value("code", "") == "output_closed";
  }
  EXPECT_TRUE(publication_failure);
}

TEST(WorkspaceCliTmux, PartialLoadReportsFailureAndPreservesCompletedSession) {
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto directory = fixture->socket_path().parent_path();
  const auto first = directory / "first.yaml";
  const auto second = directory / "second.yaml";
  std::ofstream{first} << "session_name: completed\nwindows: [{}]\n";
  std::ofstream{second} << "session_name: failed\n"
                           "windows: [{options: {not-a-window-option: 'on'}}]\n";
  const auto result = invoke({"load", first.string(), second.string(), "-s", "renamed",
                              "-d", "-S", fixture->socket_path().string(), "--ndjson"});
  ASSERT_EQ(result.code, 1);
  std::istringstream records{result.out};
  std::string line;
  Json terminal;
  int count{};
  while (std::getline(records, line)) {
    const auto record = Json::parse(line);
    if (record.at("event") == "failed" || record.at("event") == "completed") {
      terminal = record;
      ++count;
    }
  }
  EXPECT_EQ(count, 1);
  EXPECT_EQ(terminal.at("event"), "failed");
  EXPECT_EQ(terminal.at("status"), "partial");
  const auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());
  EXPECT_TRUE(server->session("completed").has_value());
  EXPECT_FALSE(server->session("failed").has_value());
  EXPECT_FALSE(server->session("renamed").has_value());
  EXPECT_TRUE(server->session("libtmux_test").has_value());
  EXPECT_EQ(Json::parse(result.err).at("code"), "tmux_failed");
}

TEST(WorkspaceCliTmux, FlushesBeforeCreationAndStopsWhenOutputCloses) {
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto file = fixture->socket_path().parent_path() / "stream\033\t.yaml";
  std::ofstream{file} << "session_name: streamed\nwindows: [{}]\n";
  const auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());
  struct Buffer : std::stringbuf {
    std::function<void()> first_flush;
    int sync() override {
      if (first_flush) {
        auto call = std::exchange(first_flush, std::function<void()>{});
        call();
      }
      return 0;
    }
  } buffer;
  bool observed{};
  buffer.first_flush = [&] {
    observed = true;
    EXPECT_FALSE(server->session("streamed").has_value());
  };
  std::ostream output{&buffer};
  std::istringstream input;
  std::ostringstream errors;
  const std::vector<std::string> arguments{
      "load", file.string(), "-d", "-S", fixture->socket_path().string(), "--ndjson"};
  EXPECT_EQ(libtmux::workspace::cli::run(arguments, input, output, errors), 0);
  EXPECT_TRUE(observed);
  EXPECT_EQ(buffer.str().find('\033'), std::string::npos);
  const auto session = server->session("streamed");
  ASSERT_TRUE(session.has_value());
  ASSERT_TRUE(session->kill().has_value());
  output.setstate(std::ios::badbit);
  EXPECT_EQ(libtmux::workspace::cli::run(arguments, input, output, errors), 1);
  EXPECT_FALSE(server->session("streamed").has_value());
}

TEST(WorkspaceCli, ProgressPreservesRedirectedBytesAndExpandsNativeCounters) {
  using namespace libtmux::workspace::cli;
  Request request{
      .command = "load",
      .importer = {},
      .values = {{"progress-format",
                  {"{session}|{window}|{session_pane_progress}|{{literal}}|{unknown}"}},
                 {"progress-lines", {"2"}}}};
  std::ostringstream out, err;
  std::pair geometry{100, 10};
  Progress progress{request, out, err, {.geometry = [&] { return geometry; }}};
  progress.event("workspace-started", {{"input", "fixture.yaml"},
                                       {"session_name", "native"},
                                       {"window_total", 1},
                                       {"session_pane_total", 2}});
  progress.event("script-output", {{"stream", "stdout"}, {"text", "raw-output\n"}});
  progress.event("script-output",
                 {{"stream", "stderr"}, {"text", "older\r\nmiddle\r\nlast\r\n"}});
  err.str("");
  progress.event("pane-completed", {{"window_name", "work"},
                                    {"window_index", 1},
                                    {"pane_index", 1},
                                    {"pane_total", 2}});
  EXPECT_EQ(out.str(), "raw-output\n");
  EXPECT_NE(err.str().find("native|work|1/2|{literal}|{unknown}"), std::string::npos);
  EXPECT_NE(err.str().find("last"), std::string::npos);
  EXPECT_EQ(err.str().find("older"), std::string::npos);
  geometry = {40, 6};
  err.str("");
  progress.event("script-output", {{"stream", "stderr"}, {"text", "resized-raw\n"}});
  EXPECT_EQ(err.str(), "\r\033[Jresized-raw\n");
  progress.finish();
}

class ProgressChild {
  int terminal_{-1}, output_{-1};
  pid_t process_{-1};
  bool interrupted_{};

public:
  enum class Output { pipe, shared_terminal, distinct_terminal };
  std::string out, err;
  explicit ProgressChild(const std::vector<std::string>& arguments,
                         Output output = Output::pipe) {
    terminal_ = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (terminal_ < 0 || ::grantpt(terminal_) != 0 || ::unlockpt(terminal_) != 0)
      throw std::runtime_error{"cannot open progress PTY"};
    const auto slave = ::open(::ptsname(terminal_), O_RDWR | O_NOCTTY | O_CLOEXEC);
    int pipe[2];
    if (slave < 0 || ::pipe(pipe) != 0)
      throw std::runtime_error{"cannot open progress output"};
    int output_slave = -1;
    if (output == Output::distinct_terminal) {
      output_ = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
      if (output_ < 0 || ::grantpt(output_) != 0 || ::unlockpt(output_) != 0)
        throw std::runtime_error{"cannot open distinct stdout PTY"};
      output_slave = ::open(::ptsname(output_), O_RDWR | O_NOCTTY | O_CLOEXEC);
      if (output_slave < 0)
        throw std::runtime_error{"cannot open distinct stdout slave"};
    }
    winsize size{12, 100, 0, 0};
    (void)::ioctl(terminal_, TIOCSWINSZ, &size);
    std::vector<std::string> command{LIBTMUX_WORKSPACE_BINARY};
    command.insert(command.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& argument : command)
      argv.push_back(argument.data());
    argv.push_back(nullptr);
    auto environment = libtmux::test::current_environment();
    libtmux::test::set_environment(environment, "TERM", "xterm-256color");
    for (const auto* name :
         {"TMUXP_PROGRESS", "TMUXP_PROGRESS_LINES", "TMUXP_PROGRESS_FORMAT"})
      libtmux::test::erase_environment(environment, name);
    std::vector<char*> envp;
    for (auto& entry : environment)
      envp.push_back(entry.data());
    envp.push_back(nullptr);
    process_ = ::fork();
    if (process_ == 0) {
      (void)::setsid();
      (void)::ioctl(slave, TIOCSCTTY, 0);
      (void)::dup2(slave, STDIN_FILENO);
      (void)::dup2(output == Output::shared_terminal     ? slave
                   : output == Output::distinct_terminal ? output_slave
                                                         : pipe[1],
                   STDOUT_FILENO);
      (void)::dup2(slave, STDERR_FILENO);
      ::close(slave);
      ::close(pipe[0]);
      ::close(pipe[1]);
      ::close(terminal_);
      if (output_slave >= 0)
        ::close(output_slave);
      if (output_ >= 0)
        ::close(output_);
      ::execve(argv.front(), argv.data(), envp.data());
      ::_exit(127);
    }
    ::close(slave);
    ::close(pipe[1]);
    if (output_slave >= 0) {
      ::close(output_slave);
      ::close(pipe[0]);
    } else {
      output_ = pipe[0];
    }
    if (process_ < 0)
      throw std::runtime_error{"cannot fork progress CLI"};
    (void)::fcntl(output_, F_SETFL, O_NONBLOCK);
    (void)::fcntl(terminal_, F_SETFL, O_NONBLOCK);
  }
  ~ProgressChild() {
    if (process_ > 0) {
      ::kill(process_, SIGKILL);
      while (::waitpid(process_, nullptr, 0) < 0 && errno == EINTR) {
      }
    }
    if (terminal_ >= 0)
      ::close(terminal_);
    if (output_ >= 0)
      ::close(output_);
  }
  ProgressChild(const ProgressChild&) = delete;
  ProgressChild& operator=(const ProgressChild&) = delete;
  void interrupt() {
    if (::kill(process_, SIGINT) != 0)
      throw std::runtime_error{"cannot interrupt progress CLI: " +
                               std::to_string(errno)};
    interrupted_ = true;
  }
  // Signals the whole process group, the way a real Ctrl-C does and
  // interrupt() above does not.
  void interrupt_group() {
    if (::kill(-process_, SIGINT) != 0)
      throw std::runtime_error{"cannot interrupt progress CLI group: " +
                               std::to_string(errno)};
    interrupted_ = true;
  }
  void resize() {
    winsize size{6, 40, 0, 0};
    (void)::ioctl(terminal_, TIOCSWINSZ, &size);
  }
  int wait(const std::function<void(ProgressChild&)>& observe = {}) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    const auto drain = [&] {
      char bytes[8192];
      for (const auto& [fd, text] :
           {std::pair{output_, &out}, std::pair{terminal_, &err}}) {
        for (;;) {
          const auto count = ::read(fd, bytes, sizeof(bytes));
          if (count <= 0)
            break;
          text->append(bytes, static_cast<std::size_t>(count));
        }
      }
    };
    for (;;) {
      drain();
      int status{};
      if (::waitpid(process_, &status, WNOHANG) == process_) {
        process_ = -1;
        drain();
        return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      }
      if (observe)
        observe(*this);
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error{"progress CLI exceeded test deadline; SIGINT sent: " +
                                 std::to_string(interrupted_) + "; stdout: " + out +
                                 "; stderr: " + err};
      pollfd descriptors[]{{output_, POLLIN, 0}, {terminal_, POLLIN, 0}};
      (void)::poll(descriptors, 2, 10);
    }
  }
};

TEST(WorkspaceCliTmux, ProgressUsesStderrAndNeverReplaysRedirectedScriptOutput) {
  Files files;
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-pg")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  std::ofstream{"before.sh"} << "printf 'RAW-STDOUT\\n'\nprintf 'RAW-STDERR\\n' >&2\n";
  for (const bool disabled : {false, true}) {
    const auto name = disabled ? "plain" : "progress";
    std::ofstream{"config.yaml"}
        << "session_name: " << name
        << "\nbefore_script: sh before.sh\nwindows:\n- window_name: work\n  panes: "
           "[{shell_command: [{cmd: 'true', sleep_after: 0.1}]}, {}]\n";
    std::vector<std::string> args{"--color",
                                  "never",
                                  "load",
                                  "config.yaml",
                                  "-d",
                                  "-S",
                                  fixture->socket_path().string(),
                                  "--progress-format",
                                  "FRAME {session} {session_pane_progress}",
                                  "--progress-lines",
                                  "2"};
    if (disabled)
      args.push_back("--no-progress");
    ProgressChild child{args};
    ASSERT_EQ(child.wait(), 0) << child.out << child.err;
    const auto first = child.out.find("RAW-STDOUT\n");
    ASSERT_NE(first, std::string::npos);
    EXPECT_EQ(child.out.find("RAW-STDOUT\n", first + 1), std::string::npos);
    EXPECT_EQ(child.out.find("FRAME"), std::string::npos);
    EXPECT_NE(child.err.find("RAW-STDERR"), std::string::npos);
    if (disabled) {
      EXPECT_EQ(child.err, "RAW-STDERR\r\n");
    } else {
      EXPECT_NE(child.err.find("FRAME progress 2/2"), std::string::npos);
      EXPECT_EQ(child.err.find("\033[36m"), std::string::npos);
      EXPECT_TRUE(child.err.ends_with("\r\033[J"));
    }
  }
}

TEST(WorkspaceCliTmux, ProgressKeepsStdoutOnItsDesignatedTerminal) {
  Files files;
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-pg")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());
  const auto keeper = server->session(fixture->session_name());
  ASSERT_TRUE(keeper.has_value());
  const auto keeper_window = keeper->active_window();
  ASSERT_TRUE(keeper_window.has_value());
  std::ofstream{"before.sh"}
      << "printf 'ROUTED-STDOUT\\n'\nprintf 'ROUTED-STDERR\\n' >&2\n";
  unsigned sequence{};
  for (const auto route :
       {ProgressChild::Output::pipe, ProgressChild::Output::shared_terminal,
        ProgressChild::Output::distinct_terminal}) {
    for (const bool enabled : {false, true}) {
      const auto name = "route" + std::to_string(sequence++);
      std::ofstream{"config.yaml"} << "session_name: " << name
                                   << "\nbefore_script: sh before.sh\nwindows: [{}]\n";
      std::vector<std::string> args{"--color",
                                    "never",
                                    "load",
                                    "config.yaml",
                                    "-d",
                                    "-S",
                                    fixture->socket_path().string()};
      if (!enabled)
        args.push_back("--no-progress");
      ProgressChild child{args, route};
      ASSERT_EQ(child.wait(), 0) << child.out << child.err;
      const auto& destination =
          route == ProgressChild::Output::shared_terminal ? child.err : child.out;
      const auto marker = destination.find("ROUTED-STDOUT");
      ASSERT_NE(marker, std::string::npos) << child.out << child.err;
      if (route != ProgressChild::Output::shared_terminal) {
        EXPECT_EQ(destination.find("ROUTED-STDOUT", marker + 1), std::string::npos);
        EXPECT_EQ(child.out.find('\033'), std::string::npos);
        EXPECT_EQ(child.out.find("Loading workspace:"), std::string::npos);
      }
      EXPECT_EQ(child.err.find("Loading workspace:") != std::string::npos, enabled);
      EXPECT_NE(child.err.find("ROUTED-STDERR"), std::string::npos);
      const auto retained = keeper_window->refresh();
      ASSERT_TRUE(retained.has_value());
      EXPECT_EQ(retained->id(), keeper_window->id());
      EXPECT_EQ(retained->layout(), keeper_window->layout());
    }
  }
}

TEST(WorkspaceCliTmux, InterruptedPaneDelayClearsProgressAndRollsBackOnlyOwnedSession) {
  Files files;
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-pg")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());
  std::ofstream{"config.yaml"} << "session_name: interrupted\nwindows:\n- panes:\n  - "
                                  "shell_command:\n    - cmd: 'touch "
                               << (files.directory / "ready").string()
                               << "'\n      sleep_after: 30\n";
  ProgressChild child{{"--color", "never", "load", "config.yaml", "-d", "-S",
                       fixture->socket_path().string()}};
  bool sent{};
  const auto code = child.wait([&](auto& running) {
    if (!sent && std::filesystem::exists("ready")) {
      running.interrupt();
      sent = true;
    }
  });
  ASSERT_TRUE(sent);
  EXPECT_EQ(code, 130) << child.err;
  EXPECT_NE(child.err.find("\r\033[JError: workspace load interrupted"),
            std::string::npos)
      << child.err;
  EXPECT_FALSE(server->session("interrupted"));
  EXPECT_TRUE(server->session(fixture->session_name()));
}

TEST(WorkspaceCliTmux, GroupInterruptNeverReportsSuccessOrLeaksARawCommand) {
  Files files;
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-pg")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());

  // Many windows keep the race live past the "ready" pane.
  std::ostringstream config;
  config << "session_name: raced\nwindows:\n- window_name: w0\n  panes:\n"
         << "  - shell_command:\n    - cmd: 'touch "
         << (files.directory / "ready").string() << "'\n";
  for (int window = 1; window <= 15; ++window) {
    config << "- window_name: w" << window << "\n  panes:\n";
    for (int pane = 0; pane < 4; ++pane)
      config << "  - printf 'x" << pane << "\\n'\n";
  }
  std::ofstream{"config.yaml"} << config.str();

  for (int attempt = 0; attempt < 6; ++attempt) {
    std::filesystem::remove("ready");
    ProgressChild child{{"--color", "never", "load", "config.yaml", "-d", "-S",
                         fixture->socket_path().string()}};
    bool sent{};
    const auto code = child.wait([&](auto& running) {
      if (!sent && std::filesystem::exists("ready")) {
        running.interrupt_group();
        sent = true;
      }
    });
    ASSERT_TRUE(sent);
    EXPECT_NE(code, 0) << "attempt " << attempt << ": " << child.err;
    EXPECT_EQ(child.err.find("(running:"), std::string::npos)
        << "attempt " << attempt << " leaked a raw tmux invocation: " << child.err;
    EXPECT_EQ(child.err.find("#{"), std::string::npos)
        << "attempt " << attempt << " leaked a format string: " << child.err;
    if (const auto session = server->session("raced"); session.has_value())
      (void)session->kill();
  }
}

TEST(WorkspaceCliTmux, LoadNdjsonEmitsTypedWindowAndPaneEvents) {
  Files files;
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-nd")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  std::ofstream{"config.yaml"} << "session_name: typed\nwindows:\n"
                                  "- window_name: w0\n  panes: [{}, {}]\n"
                                  "- window_name: w1\n  panes: [{}]\n";
  const auto result = invoke(
      {"load", "config.yaml", "-d", "-S", fixture->socket_path().string(), "--ndjson"});
  ASSERT_EQ(result.code, 0) << result.err;
  std::istringstream lines{result.out};
  std::vector<std::string> events;
  Json window_created, pane_created;
  for (std::string line; std::getline(lines, line);) {
    const auto record = Json::parse(line);
    const auto name = record.at("event").get<std::string>();
    events.push_back(name);
    if (name == "window-created" && window_created.is_null())
      window_created = record;
    if (name == "pane-created" && pane_created.is_null())
      pane_created = record;
  }
  // SPEC 2's fixed vocabulary replaces the untyped events cxx used to send.
  EXPECT_EQ(std::ranges::count(events, "build-progress"), 0);
  for (const auto* required :
       {"window-created", "window-completed", "pane-created", "pane-completed"})
    EXPECT_NE(std::ranges::find(events, required), events.end()) << required;
  ASSERT_FALSE(window_created.is_null());
  for (const auto* field : {"input_index", "session_id", "window_id", "window_index"})
    EXPECT_TRUE(window_created.contains(field)) << field;
  EXPECT_TRUE(window_created.at("window_id").get<std::string>().starts_with('@'))
      << window_created;
  ASSERT_FALSE(pane_created.is_null());
  for (const auto* field :
       {"input_index", "session_id", "window_id", "pane_id", "pane_index"})
    EXPECT_TRUE(pane_created.contains(field)) << field;
  EXPECT_TRUE(pane_created.at("pane_id").get<std::string>().starts_with('%'))
      << pane_created;
  EXPECT_TRUE(pane_created.at("session_id").get<std::string>().starts_with('$'))
      << pane_created;
}

TEST(WorkspaceCliTmux, ProgressSinkFailureReportsBorrowedWindowsAndOriginalStatus) {
  Files files;
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-pg")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());
  const auto borrowed = server->session(fixture->session_name());
  ASSERT_TRUE(borrowed.has_value());
  const auto pane = borrowed->active_pane();
  ASSERT_TRUE(pane.has_value());
  const auto daemon = pane->expand("#{pid}");
  ASSERT_TRUE(daemon.has_value());
  libtmux::test::EnvironmentGuard tmux{"TMUX", fixture->socket_path().string() + "," +
                                                   *daemon + ",0"};
  libtmux::test::EnvironmentGuard current_pane{"TMUX_PANE", pane->id()};
  for (const bool append : {false, true}) {
    const auto previous = borrowed->windows();
    ASSERT_TRUE(previous.has_value());
    std::ofstream{"config.yaml"} << "session_name: observed\nwindows: [{window_name: "
                                    "observed, panes: [{}, {}]}]\n";
    libtmux::workspace::cli::Request request{
        .command = "load",
        .importer = {},
        .values = {{"workspace-file", {"config.yaml"}},
                   {"S", {fixture->socket_path().string()}},
                   {append ? "append" : "d", {"true"}}},
        .ndjson = true};
    bool refused{};
    const auto result =
        libtmux::workspace::cli::execute(request, [&](const auto& event,
                                                      const auto& /* data */) {
          if (event == "pane-completed") {
            refused = true;
            throw libtmux::workspace::cli::Failure{77, "TEST_SINK_CLOSED",
                                                   "progress consumer closed"};
          }
        }).value;
    ASSERT_TRUE(refused);
    ASSERT_EQ(result.at("errors").size(), 1U);
    EXPECT_EQ(result.at("exit_code"), 77);
    EXPECT_EQ(result.at("errors")[0].at("code"), "TEST_SINK_CLOSED");
    EXPECT_FALSE(server->session("=observed:"));
    const auto after = borrowed->windows();
    ASSERT_TRUE(after.has_value());
    if (append) {
      EXPECT_EQ(result.at("status"), "partial");
      const auto& retained = result.at("errors")[0].at("retained_state");
      EXPECT_EQ(retained.at("session_id"), borrowed->id());
      ASSERT_EQ(retained.at("window_ids").size(), 1U);
      EXPECT_EQ(after->size(), previous->size() + 1);
      const auto id = retained.at("window_ids")[0].template get<std::string>();
      EXPECT_TRUE(std::any_of(after->begin(), after->end(),
                              [&](const auto& window) { return window.id() == id; }));
    } else {
      EXPECT_EQ(result.at("status"), "error");
      EXPECT_EQ(after->size(), previous->size());
    }
  }
}

TEST(WorkspaceCliTmux, ProgressMachineStreamsStayStructuredAndResizeRestoresRawStderr) {
  Files files;
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-pg")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  for (const bool machine : {false, true}) {
    const auto name = machine ? "machine" : "resized";
    std::ofstream{"before.sh"}
        << "printf 'READY\\n'\nsleep 0.1\nprintf 'AFTER-RESIZE\\n' >&2\n";
    std::ofstream{"config.yaml"} << "session_name: " << name
                                 << "\nbefore_script: sh before.sh\nwindows: [{}]\n";
    std::vector<std::string> args{"--color",
                                  "always",
                                  "load",
                                  "config.yaml",
                                  "-d",
                                  "-S",
                                  fixture->socket_path().string()};
    if (machine)
      args.push_back("--ndjson");
    ProgressChild child{args};
    bool resized{};
    ASSERT_EQ(child.wait([&](auto& process) {
      if (!machine && !resized && process.out.find("READY") != std::string::npos) {
        resized = true;
        process.resize();
      }
    }),
              0)
        << child.out << child.err;
    if (machine) {
      EXPECT_TRUE(child.err.empty());
      EXPECT_EQ(child.out.find('\033'), std::string::npos);
      std::istringstream records{child.out};
      std::string line;
      Json last;
      while (std::getline(records, line))
        last = Json::parse(line);
      EXPECT_EQ(last.at("event"), "completed");
      EXPECT_EQ(last.at("results")[0].at("script_output").at("stdout"), "READY\n");
      EXPECT_EQ(last.at("results")[0].at("script_output").at("stderr"),
                "AFTER-RESIZE\n");
    } else {
      EXPECT_TRUE(resized);
      EXPECT_TRUE(child.err.ends_with("\r\033[JAFTER-RESIZE\r\n")) << child.err;
    }
  }
}

TEST(WorkspaceCli, ProgressUsesUtf8CellsAndNativeEnvironmentValidation) {
  using namespace libtmux::workspace::cli;
  Request request{
      .command = "load",
      .importer = {},
      .values = {{"progress-format", {"{session}"}}, {"progress-lines", {"0"}}}};
  std::ostringstream out, err;
  Progress progress{request, out, err, {.geometry = [] { return std::pair{5, 4}; }}};
  progress.event("workspace-started", {{"input", "fixture"},
                                       {"session_name", "中中a"},
                                       {"window_total", 1},
                                       {"session_pane_total", 1}});
  EXPECT_EQ(err.str(), "中中\n\033[1A");
  progress.finish();
  libtmux::test::EnvironmentGuard lines{"TMUXP_PROGRESS_LINES", "-2"};
  const auto invalid = invoke({"load", "missing.yaml", "-d", "--json"});
  EXPECT_EQ(invalid.code, 2);
  EXPECT_EQ(Json::parse(invalid.err).at("code"), "usage");
  const auto explicit_lines =
      invoke({"load", "missing.yaml", "-d", "--json", "--progress-lines", "0"});
  EXPECT_NE(explicit_lines.code, 2);
}

TEST(WorkspaceCliTmux, FailedTerminalLoadRetainsOnlyBoundedScriptContext) {
  Files files;
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-pg")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  std::ofstream{"before.sh"}
      << "printf 'old\\nmiddle\\nFAILURE-CONTEXT\\n' >&2\nexit 7\n";
  std::ofstream{"config.yaml"}
      << "session_name: failed\nbefore_script: sh before.sh\nwindows: [{}]\n";
  ProgressChild child{{"--color", "never", "load", "config.yaml", "-d", "-S",
                       fixture->socket_path().string(), "--progress-lines", "1"},
                      ProgressChild::Output::shared_terminal};
  EXPECT_EQ(child.wait(), 1) << child.out << child.err;
  EXPECT_TRUE(child.out.empty());
  const auto cleared = child.err.rfind("\r\033[J");
  ASSERT_NE(cleared, std::string::npos) << child.err;
  EXPECT_EQ(child.err.substr(cleared),
            "\r\033[JFAILURE-CONTEXT\r\nError: before_script exited with status 7\r\n");
  const auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());
  EXPECT_FALSE(server->session("=failed:"));
  EXPECT_TRUE(server->session(fixture->session_name()));
}

} // namespace
