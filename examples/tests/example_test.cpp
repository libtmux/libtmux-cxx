// The examples, tested the only way a program can be: by running it.
//
// A consumer of the installed package — `libtmux::testing` and nothing from
// this repository's build tree. Three assertions per example: it exits zero,
// its output still says what the prose around it claims, and it leaves no
// tmux server and no directory behind.
//
// The last one works because an example takes its private tree from the same
// fixture this harness uses, and a fixture tree lives under `$TMPDIR`. Point
// `TMPDIR` at a directory the harness owns and the example's server has
// nowhere else to go, so afterwards that directory is empty or it leaked.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <libtmux/libtmux.hpp>
#include <libtmux/testing/scoped_server.hpp>
#include <libtmux/testing/tmux_version.hpp>

#include <unistd.h>

#include "run_program.hpp"

namespace {

using libtmux::test::ScopedTmuxServer;
using libtmux::test::ScopedTmuxServerOptions;
using libtmux::test::SocketNamespace;
using json = nlohmann::json;

std::filesystem::path example_binary(std::string_view name) {
  return std::filesystem::path{LIBTMUX_EXAMPLE_BINARY_DIR} /
         ("libtmux_example_" + std::string{name});
}

std::filesystem::path tmux_binary() {
  const char* const path = std::getenv("PATH");
  EXPECT_NE(path, nullptr);
  if (path == nullptr) {
    return {};
  }
  for (std::string_view entry{path};;) {
    const std::size_t separator = entry.find(':');
    const std::string_view directory = entry.substr(0, separator);
    const std::filesystem::path candidate =
        std::filesystem::path{directory.empty() ? "." : directory} / "tmux";
    std::error_code error;
    if (std::filesystem::is_regular_file(candidate, error) && !error &&
        ::access(candidate.c_str(), X_OK) == 0) {
      const auto resolved = std::filesystem::canonical(candidate, error);
      EXPECT_FALSE(error) << error.message();
      return resolved;
    }
    if (separator == std::string_view::npos) {
      break;
    }
    entry.remove_prefix(separator + 1U);
  }
  ADD_FAILURE() << "PATH does not resolve tmux";
  return {};
}

struct ExampleRun {
  int exit_code{};
  std::string output;
  std::vector<std::filesystem::path> leaked;
};

// The label this harness gives the examples it runs. Unique per process, so
// two of these in parallel do not read each other's leftovers.
std::string harness_namespace() { return "ex" + std::to_string(::getpid()); }

// Fixture trees this harness's runs have left in the temporary directory.
//
// Found by name rather than by sandboxing the child into a directory of our
// own: a directory would add its length to a socket path that must fit in
// `sockaddr_un::sun_path`, and on macOS `$TMPDIR` has already spent around
// sixty of the 104 available.
std::vector<std::filesystem::path> trees_left_behind() {
  const std::string prefix = "libtmux-cxx-" + harness_namespace();
  std::vector<std::filesystem::path> found;
  std::error_code listing;
  for (const auto& entry : std::filesystem::directory_iterator{
           std::filesystem::temp_directory_path(), listing}) {
    if (entry.path().filename().string().starts_with(prefix)) {
      found.push_back(entry.path());
    }
  }
  return found;
}

ExampleRun
run_example(std::string_view name,
            std::vector<std::pair<std::string_view, std::optional<std::string_view>>>
                overrides = {}) {
  auto environment = libtmux::test::current_environment();
  libtmux::test::erase_environment(environment, "TMUX");
  libtmux::test::erase_environment(environment, "TMUX_PANE");
  libtmux::test::set_environment(environment, "LIBTMUX_EXAMPLE_NAMESPACE",
                                 harness_namespace());
  for (const auto name : {"LIBTMUX_ARENA_DESCRIPTOR", "LIBTMUX_ARENA_ARTIFACT",
                          "LIBTMUX_SOCKET_PATH", "LIBTMUX_TMUX_BIN"}) {
    libtmux::test::erase_environment(environment, name);
  }
  for (const auto& [name, value] : overrides) {
    if (value.has_value()) {
      libtmux::test::set_environment(environment, name, *value);
    } else {
      libtmux::test::erase_environment(environment, name);
    }
  }

  const auto before = trees_left_behind();
  auto finished = libtmux::examples::run_program(example_binary(name), environment,
                                                 std::chrono::seconds{60});
  auto after = trees_left_behind();

  std::vector<std::filesystem::path> leaked;
  for (auto& path : after) {
    if (std::find(before.begin(), before.end(), path) == before.end()) {
      leaked.push_back(std::move(path));
    }
  }

  EXPECT_TRUE(finished.has_value()) << finished.error();
  if (!finished.has_value()) {
    return {};
  }
  return {finished->exit_code, finished->output, std::move(leaked)};
}

libtmux::Server connect(const ScopedTmuxServer& fixture) {
  auto server = libtmux::Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value()) << server.error().diagnostic;
  return *std::move(server);
}

std::string arena_evidence(const std::string& output) {
  constexpr std::string_view marker{"LIBTMUX_ARENA_EVIDENCE="};
  const std::size_t begin = output.find(marker);
  EXPECT_NE(begin, std::string::npos) << output;
  if (begin == std::string::npos) {
    return {};
  }
  const std::size_t end = output.find('\n', begin);
  EXPECT_NE(end, std::string::npos) << output;
  if (end == std::string::npos) {
    return {};
  }
  return output.substr(begin + marker.size(), end - begin - marker.size());
}

class Example : public testing::TestWithParam<std::string_view> {};

TEST_P(Example, SucceedsAgainstALiveTmux) {
  const auto run = run_example(GetParam());
  EXPECT_EQ(run.exit_code, 0) << run.output;
}

TEST_P(Example, LeavesNoServerAndNoDirectory) {
  const auto run = run_example(GetParam());
  std::string left;
  for (const auto& path : run.leaked) {
    left += path.string() + '\n';
  }
  EXPECT_TRUE(run.leaked.empty()) << "the example left its private tree behind:\n"
                                  << left;
}

INSTANTIATE_TEST_SUITE_P(All, Example,
                         testing::Values("01_tour", "02_workspace", "03_filter",
                                         "04_errors", "05_readme", "06_streaming"),
                         [](const auto& info) { return std::string{info.param}; });

TEST(TourOutput, NamesTheSessionItCreated) {
  const auto run = run_example("01_tour");
  ASSERT_EQ(run.exit_code, 0) << run.output;
  EXPECT_NE(run.output.find("example"), std::string::npos) << run.output;
}

TEST(TourArena, AliasesWithoutDescriptorUseAPrivateServer) {
  auto arena = ScopedTmuxServer::start(
      {.socket_namespace = SocketNamespace::consumer("tour-arena-alias")});
  ASSERT_TRUE(arena.has_value()) << arena.error();
  const libtmux::Server server = connect(*arena);

  const auto run =
      run_example("01_tour", {{"LIBTMUX_ARENA_ARTIFACT", "libtmux_example_01_tour"},
                              {"LIBTMUX_SOCKET_PATH", arena->socket_path().string()},
                              {"LIBTMUX_TMUX_BIN", "tmux"}});

  EXPECT_EQ(run.exit_code, 0) << run.output;
  EXPECT_EQ(run.output.find("LIBTMUX_ARENA_EVIDENCE="), std::string::npos)
      << run.output;
  EXPECT_TRUE(arena->is_alive());
  const auto sessions = server.sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  ASSERT_EQ(sessions->size(), 1U);
}

TEST(TourArena, RejectsIncompleteOrMismatchedContracts) {
  constexpr std::array cases{
      std::pair{"LIBTMUX_ARENA_ARTIFACT", std::optional<std::string_view>{}},
      std::pair{"LIBTMUX_SOCKET_PATH", std::optional<std::string_view>{}},
      std::pair{"LIBTMUX_TMUX_BIN", std::optional<std::string_view>{}},
      std::pair{"LIBTMUX_ARENA_ARTIFACT", std::optional<std::string_view>{"other"}},
  };
  for (const auto& [name, value] : cases) {
    SCOPED_TRACE(name);
    const auto run =
        run_example("01_tour", {{"LIBTMUX_ARENA_DESCRIPTOR", "borrow"},
                                {"LIBTMUX_ARENA_ARTIFACT", "libtmux_example_01_tour"},
                                {"LIBTMUX_SOCKET_PATH", "/not-a-tmux-socket"},
                                {"LIBTMUX_TMUX_BIN", "tmux"},
                                {name, value}});
    EXPECT_NE(run.exit_code, 0) << run.output;
    EXPECT_NE(run.output.find("incomplete or mismatched arena contract"),
              std::string::npos)
        << run.output;
  }
}

TEST(TourArena, RejectsAClientBinaryOutsidePathBeforeContactingTheServer) {
  auto arena = ScopedTmuxServer::start(
      {.socket_namespace = SocketNamespace::consumer("tour-arena-client")});
  ASSERT_TRUE(arena.has_value()) << arena.error();

  const auto run =
      run_example("01_tour", {{"LIBTMUX_ARENA_DESCRIPTOR", "borrow"},
                              {"LIBTMUX_ARENA_ARTIFACT", "libtmux_example_01_tour"},
                              {"LIBTMUX_SOCKET_PATH", arena->socket_path().string()},
                              {"LIBTMUX_TMUX_BIN", "/not-an-arena-client/tmux"}});

  EXPECT_NE(run.exit_code, 0) << run.output;
  EXPECT_NE(run.output.find("arena tmux binary is not PATH's tmux"), std::string::npos)
      << run.output;
  EXPECT_TRUE(arena->is_alive());
}

TEST(TourArena, RejectsADirectoryNamedTmuxBeforeTheClientOnPath) {
  auto arena = ScopedTmuxServer::start(
      {.socket_namespace = SocketNamespace::consumer("tour-arena-directory")});
  ASSERT_TRUE(arena.has_value()) << arena.error();
  const libtmux::Server server = connect(*arena);
  ASSERT_TRUE(
      server.set_global_option("@libtmux_arena_challenge", "directory").has_value());

  const std::filesystem::path decoy_parent = arena->tmux_tmpdir() / "arena-client";
  std::error_code error;
  ASSERT_TRUE(std::filesystem::create_directory(decoy_parent, error))
      << error.message();
  ASSERT_TRUE(std::filesystem::create_directory(decoy_parent / "tmux", error))
      << error.message();
  const std::filesystem::path client = tmux_binary();
  const char* const inherited_path = std::getenv("PATH");
  ASSERT_NE(inherited_path, nullptr);
  const std::string path = decoy_parent.string() + ":" + client.parent_path().string() +
                           ":" + inherited_path;
  const std::string directory_tmux = (decoy_parent / "tmux").string();

  const auto run =
      run_example("01_tour", {{"LIBTMUX_ARENA_DESCRIPTOR", "borrow"},
                              {"LIBTMUX_ARENA_ARTIFACT", "libtmux_example_01_tour"},
                              {"LIBTMUX_SOCKET_PATH", arena->socket_path().string()},
                              {"LIBTMUX_TMUX_BIN", directory_tmux},
                              {"PATH", path}});

  EXPECT_NE(run.exit_code, 0) << run.output;
  EXPECT_NE(run.output.find("arena tmux binary is not PATH's tmux"), std::string::npos)
      << run.output;
  EXPECT_TRUE(arena->is_alive());
}

TEST(TourArena, RunsAgainstABorrowedServerAndEmitsEvidence) {
  auto arena = ScopedTmuxServer::start(
      {.socket_namespace = SocketNamespace::consumer("tour-arena-run")});
  ASSERT_TRUE(arena.has_value()) << arena.error();
  const libtmux::Server server = connect(*arena);
  const std::string challenge{"quote \" slash \\\x80"};
  ASSERT_TRUE(
      server.set_global_option("@libtmux_arena_challenge", challenge).has_value());

  const auto run =
      run_example("01_tour", {{"LIBTMUX_ARENA_DESCRIPTOR", "borrow"},
                              {"LIBTMUX_ARENA_ARTIFACT", "libtmux_example_01_tour"},
                              {"LIBTMUX_SOCKET_PATH", arena->socket_path().string()},
                              {"LIBTMUX_TMUX_BIN", tmux_binary().string()}});

  ASSERT_EQ(run.exit_code, 0) << run.output;
  const std::string evidence = arena_evidence(run.output);
  const json parsed = json::parse(evidence, nullptr, false);
  ASSERT_FALSE(parsed.is_discarded()) << evidence;
  ASSERT_TRUE(parsed.is_object());
  EXPECT_EQ(parsed.at("schema"), 1);
  EXPECT_EQ(parsed.at("server_pid"), arena->server_pid());
  EXPECT_EQ(parsed.at("socket_path"), arena->socket_path().string());
  EXPECT_TRUE(parsed.at("challenge").is_string());
  EXPECT_EQ(parsed.at("artifact"), "libtmux_example_01_tour");
  EXPECT_TRUE(arena->is_alive());
  EXPECT_TRUE(server.sessions().has_value());
}

TEST(Package, ReportsTheRunningTmuxToAConsumer) {
  const auto described = libtmux::test::describe_running_tmux();
  EXPECT_NE(described, "unknown");
  EXPECT_EQ(described.rfind("tmux ", 0), 0U) << described;
}

TEST(Package, NamespacedServersDoNotCollide) {
  auto mine = ScopedTmuxServer::start(
      ScopedTmuxServerOptions{.socket_namespace = SocketNamespace::consumer("alpha")});
  auto theirs = ScopedTmuxServer::start(
      ScopedTmuxServerOptions{.socket_namespace = SocketNamespace::consumer("beta")});
  ASSERT_TRUE(mine.has_value()) << mine.error();
  ASSERT_TRUE(theirs.has_value()) << theirs.error();

  EXPECT_NE(mine->socket_path(), theirs->socket_path());
  EXPECT_NE(mine->server_pid(), theirs->server_pid());
  EXPECT_NE(mine->socket_path().string().find("alpha"), std::string::npos)
      << mine->socket_path();
  EXPECT_NE(theirs->socket_path().string().find("beta"), std::string::npos)
      << theirs->socket_path();
}

TEST(Package, RefusesANamespaceThatWouldNotSurviveAPath) {
  const auto refused = ScopedTmuxServer::start(
      ScopedTmuxServerOptions{.socket_namespace = {.label = "has/slash"}});
  ASSERT_FALSE(refused.has_value());
  EXPECT_NE(refused.error().find("A-Za-z0-9._-"), std::string::npos) << refused.error();
}

} // namespace
