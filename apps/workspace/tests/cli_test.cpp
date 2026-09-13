#include "../src/progress.hpp"
#include "../src/services.hpp"
#include "workspace_cli.hpp"

#include <algorithm>
#include <cerrno>
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

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "libtmux/server.hpp"
#include "libtmux/testing/environment_guard.hpp"
#include "libtmux/testing/scoped_server.hpp"

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
    EXPECT_EQ(error.at("code"), "USAGE");
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
    EXPECT_EQ(Json::parse(machine.err).at("code"), "USAGE");
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
        EXPECT_EQ(Json::parse(result.err).at("code"), "BEFORE_SCRIPT_FAILED");
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
    EXPECT_NE(result.err.find("LOG_FILE_UNAVAILABLE"), std::string::npos);
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
  EXPECT_EQ(Json::parse(refused.err).at("code"), "DESTINATION_EXISTS");
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
  EXPECT_EQ(Json::parse(result.err).at("code"), "PROCESS_FAILED");
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
  EXPECT_EQ(Json::parse(captured.err).at("code"), "CAPTURE_LOSSY");
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
                         throw libtmux::workspace::cli::Failure{1, "OUTPUT_CLOSED",
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
              closed ? "OUTPUT_CLOSED" : "OPERATION_FAILED");
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
                             1, "OUTPUT_CLOSED", "script event sink closed"};
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
    publication_failure |= record.value("code", "") == "OUTPUT_CLOSED";
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
  EXPECT_EQ(Json::parse(result.err).at("code"), "BUILD_FAILED");
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
  progress.event("build-progress", {{"phase", "pane-completed"},
                                    {"window_name", "work"},
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
  std::string out, err;
  explicit ProgressChild(const std::vector<std::string>& arguments,
                         bool stdout_terminal = false) {
    terminal_ = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (terminal_ < 0 || ::grantpt(terminal_) != 0 || ::unlockpt(terminal_) != 0)
      throw std::runtime_error{"cannot open progress PTY"};
    const auto slave = ::open(::ptsname(terminal_), O_RDWR | O_NOCTTY | O_CLOEXEC);
    int pipe[2];
    if (slave < 0 || ::pipe(pipe) != 0)
      throw std::runtime_error{"cannot open progress output"};
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
      (void)::dup2(stdout_terminal ? slave : pipe[1], STDOUT_FILENO);
      (void)::dup2(slave, STDERR_FILENO);
      ::close(slave);
      ::close(pipe[0]);
      ::close(pipe[1]);
      ::close(terminal_);
      ::execve(argv.front(), argv.data(), envp.data());
      ::_exit(127);
    }
    ::close(slave);
    ::close(pipe[1]);
    output_ = pipe[0];
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
                                                      const auto& data) {
          if (event == "build-progress" && data.at("phase") == "pane-completed") {
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
  EXPECT_EQ(Json::parse(invalid.err).at("code"), "USAGE");
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
                      true};
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
