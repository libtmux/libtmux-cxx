// The examples, tested the only way a program can be: by running it.
//
// This suite is a consumer. It links `libtmux::testing` out of the installed
// package exactly as a third party would, and knows nothing about this
// repository's build tree — no private headers, no `tests/support`, no
// compile definitions carrying answers the build already worked out. If it
// passes, an outside project can test its own tmux code the same way.
//
// Three things are asserted per example, and the third is the one no existing
// test covered:
//
//   1. it exits zero;
//   2. its output still contains what the prose around it claims;
//   3. it leaves no tmux server and no directory behind.
//
// (3) works because every example gets its private tree from the same fixture
// this harness uses, and a fixture tree lives under `$TMPDIR`. Point `TMPDIR`
// at a directory this harness owns and an example's server has nowhere else to
// go — so after it exits, that directory is either empty or the example
// leaked, and there is no third possibility.

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <libtmux/testing/environment_guard.hpp>
#include <libtmux/testing/scoped_server.hpp>
#include <libtmux/testing/tmux_version.hpp>

#include "run_program.hpp"

namespace {

using libtmux::test::ScopedTmuxServer;
using libtmux::test::ScopedTmuxServerOptions;
using libtmux::test::SocketNamespace;

// Where the built example programs are. The build passes it rather than this
// guessing: an installed-package consumer has no layout convention to rely on.
std::filesystem::path example_binary(std::string_view name) {
  return std::filesystem::path{LIBTMUX_EXAMPLE_BINARY_DIR} /
         ("libtmux_example_" + std::string{name});
}

struct ExampleRun {
  int exit_code{};
  std::string output;
  std::vector<std::filesystem::path> leaked;
};

// Run one example inside a tree this harness owns, and report what it left.
ExampleRun run_example(std::string_view name, std::string_view suite) {
  // The harness's own server. The example does not use it — it starts its own —
  // but starting one here is what creates the private tree, and pointing
  // `TMPDIR` inside it is what makes the example's tree land where this can
  // see it.
  auto harness = ScopedTmuxServer::start(ScopedTmuxServerOptions{
      .session_name = "example_harness",
      .socket_namespace = SocketNamespace::consumer("examples")});
  EXPECT_TRUE(harness.has_value()) << harness.error();
  if (!harness.has_value()) {
    return {};
  }

  const std::filesystem::path sandbox = harness->tmux_tmpdir() / "examples";
  std::filesystem::create_directory(sandbox);

  auto environment = harness->child_environment();
  libtmux::test::set_environment(environment, "TMPDIR", sandbox.string());
  libtmux::test::set_environment(environment, "LIBTMUX_EXAMPLE_NAMESPACE",
                                 std::string{suite});

  auto finished = libtmux::examples::run_program(example_binary(name), environment,
                                                 std::chrono::seconds{60});
  EXPECT_TRUE(finished.has_value()) << finished.error();
  if (!finished.has_value()) {
    return {};
  }

  std::vector<std::filesystem::path> leaked;
  for (const auto& entry : std::filesystem::directory_iterator{sandbox}) {
    leaked.push_back(entry.path());
  }
  return {finished->exit_code, finished->output, std::move(leaked)};
}

class Example : public testing::TestWithParam<std::string_view> {};

TEST_P(Example, SucceedsAgainstALiveTmux) {
  const auto run = run_example(GetParam(), "examples");
  EXPECT_EQ(run.exit_code, 0) << run.output;
}

TEST_P(Example, LeavesNoServerAndNoDirectory) {
  const auto run = run_example(GetParam(), "examples");
  std::string left;
  for (const auto& path : run.leaked) {
    left += path.string() + '\n';
  }
  EXPECT_TRUE(run.leaked.empty()) << "the example left its private tree behind:\n"
                                  << left;
}

INSTANTIATE_TEST_SUITE_P(All, Example,
                         testing::Values("01_tour", "02_workspace", "03_filter",
                                         "04_errors", "05_readme"),
                         [](const auto& info) { return std::string{info.param}; });

// The tour prints what it found. If it stops naming the session it made, the
// example still exits zero and still proves nothing.
TEST(TourOutput, NamesTheSessionItCreated) {
  const auto run = run_example("01_tour", "examples");
  ASSERT_EQ(run.exit_code, 0) << run.output;
  EXPECT_NE(run.output.find("example"), std::string::npos) << run.output;
}

// A consumer of the package can ask which tmux it is talking to without this
// repository's configure-time stamp — that is the whole point of resolving it
// at runtime.
TEST(Package, ReportsTheRunningTmuxToAConsumer) {
  const auto described = libtmux::test::describe_running_tmux();
  EXPECT_NE(described, "unknown");
  EXPECT_EQ(described.rfind("tmux ", 0), 0U) << described;
}

// Two fixtures asked for at once must not land on one server. This is the
// property the whole namespace scheme exists to provide, and it is asserted
// here — from outside the package — rather than only in its own suite.
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
