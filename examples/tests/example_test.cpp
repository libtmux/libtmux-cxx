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
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <libtmux/testing/scoped_server.hpp>
#include <libtmux/testing/tmux_version.hpp>

#include <unistd.h>

#include "run_program.hpp"

namespace {

using libtmux::test::ScopedTmuxServer;
using libtmux::test::ScopedTmuxServerOptions;
using libtmux::test::SocketNamespace;

std::filesystem::path example_binary(std::string_view name) {
  return std::filesystem::path{LIBTMUX_EXAMPLE_BINARY_DIR} /
         ("libtmux_example_" + std::string{name});
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

ExampleRun run_example(std::string_view name) {
  auto environment = libtmux::test::current_environment();
  libtmux::test::erase_environment(environment, "TMUX");
  libtmux::test::erase_environment(environment, "TMUX_PANE");
  libtmux::test::set_environment(environment, "LIBTMUX_EXAMPLE_NAMESPACE",
                                 harness_namespace());

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
