// Running a shell command through the server, and reading a config file.
//
// Both reach state a caller cannot reach for itself: the machine and the
// environment the server is on, and tmux's own parser.

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "libtmux/entities.hpp"
#include "libtmux/server.hpp"
#include "support/scoped_tmux_server.hpp"

namespace {

using namespace std::chrono_literals;
using libtmux::Server;

Server connect(const libtmux::test::ScopedTmuxServer& fixture) {
  auto server = Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value());
  return server.value();
}

// Whether the server holds a global option by this name, whatever its value.
bool holds_option(const Server& server, std::string_view name) {
  const auto options = server.global_options();
  EXPECT_TRUE(options.has_value());
  if (!options.has_value()) {
    return false;
  }
  return std::ranges::any_of(*options, [name](const libtmux::OptionEntry& entry) {
    return entry.name == name;
  });
}

TEST(Scripting, AShellCommandRunsOnTheServersMachine) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const auto evidence = fixture->tmux_tmpdir() / "it-ran";
  ASSERT_FALSE(std::filesystem::exists(evidence));

  ASSERT_TRUE(server.run_shell("touch " + evidence.string()).has_value());

  // The file is the whole proof: tmux discards what the command printed on
  // two of the supported versions, so the effect is what can be asserted.
  EXPECT_TRUE(std::filesystem::exists(evidence));
}

TEST(Scripting, AFailingCommandCarriesItsExitCode) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto failed = server.run_shell("exit 3");
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error().exit_code, 3);
  EXPECT_TRUE(failed.error().dispatched);

  // 127 is the shell's own answer for a command it could not find, and it
  // arrives the same way: as the status, not as a different kind of error.
  const auto absent = server.run_shell("no-such-binary-in-this-test");
  ASSERT_FALSE(absent.has_value());
  EXPECT_EQ(absent.error().exit_code, 127);

  // Nothing is dispatched for a command that is not one.
  const auto empty = server.run_shell("");
  ASSERT_FALSE(empty.has_value());
  EXPECT_EQ(empty.error().kind, libtmux::FailureKind::validation);
  EXPECT_FALSE(empty.error().dispatched);
}

TEST(Scripting, ABackgroundedCommandRunsWithoutBeingWaitedFor) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const auto evidence = fixture->tmux_tmpdir() / "later";

  // Failing and backgrounded: the call succeeds because tmux stops waiting
  // once the command has started, which is the difference being asserted.
  ASSERT_TRUE(
      server.run_shell("touch " + evidence.string() + "; exit 3", /*background=*/true)
          .has_value());

  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!std::filesystem::exists(evidence) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(5ms);
  }
  EXPECT_TRUE(std::filesystem::exists(evidence));

  // The same command in the foreground reports what it did.
  EXPECT_FALSE(server.run_shell("exit 3").has_value());
}

TEST(Scripting, SourcingAFileAppliesWhatIsInIt) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const auto config = fixture->tmux_tmpdir() / "sourced.conf";
  {
    std::ofstream writing{config};
    writing << "set-option -g @sourced yes\n";
  }
  ASSERT_FALSE(holds_option(server, "@sourced"));

  ASSERT_TRUE(server.source_file(config).has_value());

  EXPECT_TRUE(holds_option(server, "@sourced"));
}

TEST(Scripting, CheckingAFileRunsNoneOfIt) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const auto config = fixture->tmux_tmpdir() / "checked.conf";
  {
    std::ofstream writing{config};
    writing << "set-option -g @checked yes\n";
  }

  ASSERT_TRUE(server.check_file(config).has_value());

  // The same file sourced would set it. That it is still absent is what
  // separates checking from applying.
  EXPECT_FALSE(holds_option(server, "@checked"));
  ASSERT_TRUE(server.source_file(config).has_value());
  EXPECT_TRUE(holds_option(server, "@checked"));
}

TEST(Scripting, AFileTmuxWouldRefuseIsReportedBeforeAnythingRuns) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const auto config = fixture->tmux_tmpdir() / "broken.conf";
  {
    std::ofstream writing{config};
    writing << "set-option -g @first yes\nnot-a-tmux-command\n";
  }

  const auto refused = server.check_file(config);
  ASSERT_FALSE(refused.has_value());
  EXPECT_NE(refused.error().diagnostic.find("not-a-tmux-command"), std::string::npos);

  // Checking a broken file leaves the good line unapplied too, which is the
  // reason to check before sourcing rather than after.
  EXPECT_FALSE(holds_option(server, "@first"));
}

TEST(Scripting, AFileTheServerCannotReadIsReported) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const auto absent = fixture->tmux_tmpdir() / "no-such.conf";

  const auto sourced = server.source_file(absent);
  ASSERT_FALSE(sourced.has_value());
  EXPECT_NE(sourced.error().diagnostic.find(absent.string()), std::string::npos);

  const auto checked = server.check_file(absent);
  EXPECT_FALSE(checked.has_value());
}

} // namespace
