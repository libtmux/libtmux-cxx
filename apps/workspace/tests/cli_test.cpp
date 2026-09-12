#include "../src/services.hpp"
#include "workspace_cli.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <sstream>

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
    directory = created;
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
                         "windows: [{layout: invalid-layout}]\n";
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
  EXPECT_EQ(retained->front().name(), problem.at("retained_state").at("session_name"));
}

TEST(WorkspaceCliTmux, FailedEventDeliveryRetainsCompletedSessionAccounting) {
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli-events")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto file = fixture->socket_path().parent_path() / "events.yaml";
  std::ofstream{file} << "session_name: retained\nwindows: [{}]\n";
  const libtmux::workspace::cli::Request request{
      .command = "load",
      .importer = {},
      .values = {{"workspace-file", {file.string()}},
                 {"d", {"true"}},
                 {"S", {fixture->socket_path().string()}}},
      .ndjson = true};
  const auto result = libtmux::workspace::cli::execute(
      request, [](const std::string& event, const libtmux::workspace::cli::Json&) {
        if (event == "session-created")
          throw std::runtime_error{"event sink failed"};
      });
  EXPECT_EQ(result.at("status"), "partial");
  ASSERT_EQ(result.at("results").size(), 1U);
  EXPECT_EQ(result.at("results")[0].at("session_name"), "retained");
  const auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());
  EXPECT_TRUE(server->session("=retained:").has_value());
}

TEST(WorkspaceCliTmux, PartialLoadReportsFailureAndPreservesCompletedSession) {
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("cli")});
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto directory = fixture->socket_path().parent_path();
  const auto first = directory / "first.yaml";
  const auto second = directory / "second.yaml";
  std::ofstream{first} << "session_name: completed\nwindows: [{}]\n";
  std::ofstream{second}
      << "session_name: failed\nwindows: [{layout: invalid-layout}]\n";
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
} // namespace
