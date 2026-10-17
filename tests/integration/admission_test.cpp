// The core support floor, tmux 3.3a, and what enforces it.
//
// A client executable and the daemon it reaches can be different releases, so
// the daemon answers. These tests hold a refusal to three standards: it
// happens before the requested command has an effect, the same command is
// accepted by a qualifying daemon, and removing the check makes the refusal
// test fail.
//
// The refusal tests need a daemon older than the floor, which a supported
// build lane does not have. They read it from LIBTMUX_TEST_REJECTED_TMUX and
// report a skip otherwise; the rejection lanes in CI set it.

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "libtmux/async.hpp"
#include "libtmux/command.hpp"
#include "libtmux/control.hpp"
#include "libtmux/server.hpp"
#include "libtmux/testing/environment_guard.hpp"
#include "libtmux/testing/scoped_server.hpp"
#include "libtmux/version.hpp"

namespace {

using libtmux::CommandFailure;
using libtmux::DeliveryStatus;
using libtmux::ExecutionPolicy;
using libtmux::FailureKind;
using libtmux::Server;
using libtmux::Version;

Version parsed(std::string_view text) {
  const auto version = libtmux::parse_version("tmux " + std::string{text});
  EXPECT_TRUE(version.has_value()) << text;
  return version.value_or(Version{});
}

TEST(AdmissionVersions, TheFloorIsTheCompleteReleaseIncludingItsLetter) {
  EXPECT_EQ(libtmux::kMinimumSupported, parsed("3.3a"));
  EXPECT_FALSE(libtmux::is_supported(parsed("3.2a")));
  EXPECT_FALSE(libtmux::is_supported(parsed("3.3")));
  EXPECT_TRUE(libtmux::is_supported(parsed("3.3a")));
  EXPECT_TRUE(libtmux::is_supported(parsed("3.3b")));
  EXPECT_TRUE(libtmux::is_supported(parsed("3.4")));
  EXPECT_TRUE(libtmux::is_supported(parsed("3.10")));
  EXPECT_TRUE(libtmux::is_supported(parsed("4.0")));
  EXPECT_FALSE(libtmux::is_supported(parsed("2.9a")));
}

TEST(AdmissionVersions, DevelopmentBuildsOrderBelowTheReleaseTheyLeadTo) {
  EXPECT_LT(parsed("next-3.3a"), parsed("3.3a"));
  EXPECT_FALSE(libtmux::is_supported(parsed("next-3.3a")));
  EXPECT_TRUE(libtmux::is_supported(parsed("next-3.4")));
  EXPECT_LT(parsed("next-3.9"), parsed("3.9"));
  EXPECT_LT(parsed("3.8-rc"), parsed("3.8"));
  EXPECT_LT(parsed("3.8-rc"), parsed("3.8-rc2"));
  EXPECT_LT(parsed("3.8-rc2"), parsed("3.8"));
  EXPECT_LT(parsed("next-3.8"), parsed("3.8-rc"));
  EXPECT_GT(parsed("3.8-rc"), parsed("3.7c"));
  EXPECT_TRUE(libtmux::is_supported(parsed("3.4-rc")));
  EXPECT_TRUE(parsed("3.8-rc2") >= (Version{.major = 3, .minor = 8, .candidate = 1}));
  EXPECT_FALSE(parsed("next-3.8") >= (Version{.major = 3, .minor = 8, .candidate = 1}));
}

TEST(AdmissionVersions, TheCanonicalOrderingHoldsFromDevelopmentBuildToPatchLetter) {
  const std::vector<std::vector<std::string>> chains{
      {"next-3.7", "3.7-rc", "3.7-rc3", "3.7", "3.7a", "3.7d"},
      {"next-3.4", "3.4-rc", "3.4"},
      {"3.9", "3.10", "4.0"}};
  for (const auto& chain : chains) {
    for (std::size_t lower = 0; lower < chain.size(); ++lower) {
      for (std::size_t higher = lower + 1; higher < chain.size(); ++higher) {
        EXPECT_LT(parsed(chain[lower]), parsed(chain[higher]))
            << chain[lower] << " < " << chain[higher];
      }
    }
  }
  // The gates written as literals use that same order.
  const Version control{.major = 3, .minor = 7};
  EXPECT_GE(parsed("3.8-rc2"), control);
  EXPECT_GE(parsed("next-3.8"), control);
  EXPECT_LT(parsed("next-3.7"), control);
  EXPECT_LT(parsed("3.7-rc2"), control);
  EXPECT_LT(parsed("3.4-rc"), (Version{.major = 3, .minor = 4}));
  EXPECT_LT(parsed("3.5-rc"), (Version{.major = 3, .minor = 5}));
  EXPECT_NE(parsed("3.7-rc"), control);
  EXPECT_NE(parsed("3.7a"), control);
}

TEST(AdmissionVersions, UnreadableReportsAreErrorsAndMasterIsUnbounded) {
  for (const auto* text :
       {"3.4garbage", "9.9alpha", "3.8-rc0", "3.8-rcx", "", "3.", ".3", "three"}) {
    EXPECT_FALSE(libtmux::parse_version(std::string{"tmux "} + text).has_value())
        << text;
  }
  const auto master = libtmux::parse_version("tmux master");
  ASSERT_TRUE(master.has_value());
  EXPECT_TRUE(master->unbounded);
}

// A tmux executable on PATH, resolved the way the library resolves it.
std::filesystem::path find_on_path(std::string_view name) {
  const char* const path = std::getenv("PATH");
  if (path == nullptr) {
    return {};
  }
  std::stringstream directories{path};
  for (std::string directory; std::getline(directories, directory, ':');) {
    const auto candidate = std::filesystem::path{directory} / name;
    if (::access(candidate.c_str(), X_OK) == 0 &&
        std::filesystem::is_regular_file(candidate)) {
      return candidate;
    }
  }
  return {};
}

std::string slurp(const std::filesystem::path& path) {
  std::ifstream file{path};
  return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

std::size_t lines_in(const std::filesystem::path& path) {
  const std::string text = slurp(path);
  return static_cast<std::size_t>(std::ranges::count(text, '\n'));
}

bool wait_for_lines(const std::filesystem::path& path, std::size_t wanted) {
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (lines_in(path) >= wanted) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
  }
  return false;
}

// A 0700 directory removed with its contents.
class PrivateDirectory final {
public:
  PrivateDirectory() {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "libtmux-adm-XXXXXX").string();
    if (::mkdtemp(pattern.data()) != nullptr) {
      path_ = pattern;
    }
  }
  ~PrivateDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  PrivateDirectory(const PrivateDirectory&) = delete;
  PrivateDirectory& operator=(const PrivateDirectory&) = delete;
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

std::string shell_quote(const std::string& text) {
  std::string quoted = "'";
  for (const char character : text) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted.push_back(character);
    }
  }
  return quoted + "'";
}

// Run a command line and return its standard output.
std::string output_of(const std::vector<std::string>& argv) {
  std::string command;
  for (const std::string& argument : argv) {
    command += shell_quote(argument) + " ";
  }
  command += "2>&1";
  std::string output;
  if (FILE* const pipe = ::popen(command.c_str(), "r"); pipe != nullptr) {
    std::array<char, 512> buffer{};
    while (const std::size_t count =
               std::fread(buffer.data(), 1, buffer.size(), pipe)) {
      output.append(buffer.data(), count);
    }
    ::pclose(pipe);
  }
  return output;
}

Server server_for(const libtmux::test::ScopedTmuxServer& fixture,
                  ExecutionPolicy policy = {}) {
  auto server =
      Server::at_socket_path(fixture.socket_path().string(), {}, std::move(policy));
  EXPECT_TRUE(server.has_value());
  return server.value();
}

std::optional<std::filesystem::path> rejected_tmux() {
  if (const char* const configured = std::getenv("LIBTMUX_TEST_REJECTED_TMUX");
      configured != nullptr && *configured != '\0') {
    return std::filesystem::path{configured};
  }
  return std::nullopt;
}

// The sessions a daemon holds, asked of the daemon's own executable so that
// nothing here depends on the library under test.
std::string sessions_held_by(const libtmux::test::ScopedTmuxServer& fixture) {
  auto argv = fixture.command_prefix();
  argv.insert(argv.end(), {"list-sessions", "-F", "#{session_name}"});
  return output_of(argv);
}

// ---------------------------------------------------------------------------
// Refusal below the floor.
// ---------------------------------------------------------------------------

class RejectedDaemon : public testing::Test {
protected:
  void SetUp() override {
    const auto binary = rejected_tmux();
    if (!binary.has_value() &&
        std::getenv("LIBTMUX_REQUIRE_REJECTED_TMUX") != nullptr) {
      FAIL() << "the rejection lane must name the tmux it rejects";
    }
    if (!binary.has_value()) {
      GTEST_SKIP() << "LIBTMUX_TEST_REJECTED_TMUX names the tmux older than 3.3a "
                      "that the rejection lanes run these against";
    }
    libtmux::test::ScopedTmuxServerOptions options;
    options.tmux_binary = *binary;
    options.session_name = "sentinel";
    auto started = libtmux::test::ScopedTmuxServer::start(std::move(options));
    ASSERT_TRUE(started.has_value()) << started.error();
    fixture_ = std::make_unique<libtmux::test::ScopedTmuxServer>(std::move(*started));
    ASSERT_EQ(sessions_held_by(*fixture_), "sentinel\n");
  }

  void expect_refused(const CommandFailure& failure) {
    EXPECT_EQ(failure.kind, FailureKind::unsupported) << failure.diagnostic;
    EXPECT_EQ(failure.delivery, DeliveryStatus::not_started);
    EXPECT_NE(failure.diagnostic.find("3.3a"), std::string::npos) << failure.diagnostic;
  }

  void expect_untouched() { EXPECT_EQ(sessions_held_by(*fixture_), "sentinel\n"); }

  std::unique_ptr<libtmux::test::ScopedTmuxServer> fixture_;
};

TEST_F(RejectedDaemon, ACommandIsRefusedBeforeItHasAnEffect) {
  // The client is the supported tmux on PATH: a newer executable does not
  // qualify the older daemon it reaches.
  auto server = server_for(*fixture_);
  const auto refused = server.run({"new-session", "-d", "-s", "intruder"});
  ASSERT_FALSE(refused.has_value());
  expect_refused(refused.error());
  expect_untouched();
}

TEST_F(RejectedDaemon, EveryEntryPointIsRefusedWithNoEffect) {
  auto server = server_for(*fixture_);

  libtmux::CommandBatch batch;
  ASSERT_TRUE(batch.add(libtmux::CommandRequest{"new-session", "-d", "-s", "first"}));
  ASSERT_TRUE(batch.add(libtmux::CommandRequest{"new-session", "-d", "-s", "second"}));
  const auto batched = server.run_batch(batch);
  ASSERT_FALSE(batched.has_value());
  expect_refused(batched.error());

  const auto created = server.new_session("typed");
  ASSERT_FALSE(created.has_value());
  expect_refused(created.error());

  // A lenient list must say why it is empty rather than answer empty.
  const auto sessions = server.sessions();
  ASSERT_FALSE(sessions.has_value());
  expect_refused(sessions.error());
  ASSERT_FALSE(server.windows().has_value());
  ASSERT_FALSE(server.panes().has_value());
  ASSERT_FALSE(server.buffers().has_value());

  const auto alive = server.check_alive();
  ASSERT_FALSE(alive.has_value());
  expect_refused(alive.error());

  // kill-server is the command that would hurt: it must not reach the daemon.
  const auto killed = server.kill();
  ASSERT_FALSE(killed.has_value());
  expect_refused(killed.error());

  expect_untouched();
  EXPECT_TRUE(fixture_->is_alive());
}

TEST_F(RejectedDaemon, AnOlderClientIsRefusedToo) {
  ExecutionPolicy policy;
  policy.tmux_binary = *rejected_tmux();
  auto server = server_for(*fixture_, policy);
  const auto refused = server.run({"new-session", "-d", "-s", "intruder"});
  ASSERT_FALSE(refused.has_value());
  expect_refused(refused.error());
  expect_untouched();
}

TEST_F(RejectedDaemon, TheVersionStaysReadableForADiagnostic) {
  auto server = server_for(*fixture_);
  const auto version = server.tmux_version();
  ASSERT_TRUE(version.has_value());
  EXPECT_FALSE(libtmux::is_supported(*version));
  // The executable's own report authorizes nothing.
  const auto banner = server.run({"-V"});
  EXPECT_TRUE(banner.has_value());
  expect_untouched();
}

TEST_F(RejectedDaemon, ADaemonBelowTheFloorIsNeverAttachedToByControlMode) {
  auto server = server_for(*fixture_);
  libtmux::ConnectionOptions options;
  options.socket_path = fixture_->socket_path();
  options.session_name = "sentinel";
  const auto connection = libtmux::Connection::connect(std::move(options));
  ASSERT_FALSE(connection.has_value());
  EXPECT_NE(connection.error().message.find("3.3a"), std::string::npos)
      << connection.error().message;
  EXPECT_EQ(connection.error().delivery, DeliveryStatus::not_started);

  auto clients = fixture_->command_prefix();
  clients.insert(clients.end(), {"list-clients"});
  EXPECT_EQ(output_of(clients), "");
}

// ---------------------------------------------------------------------------
// The same commands on a qualifying daemon.
// ---------------------------------------------------------------------------

class SupportedDaemon : public testing::Test {
protected:
  void SetUp() override {
    libtmux::test::ScopedTmuxServerOptions options;
    options.session_name = "sentinel";
    auto started = libtmux::test::ScopedTmuxServer::start(std::move(options));
    ASSERT_TRUE(started.has_value()) << started.error();
    fixture_ = std::make_unique<libtmux::test::ScopedTmuxServer>(std::move(*started));
  }

  std::unique_ptr<libtmux::test::ScopedTmuxServer> fixture_;
};

TEST_F(SupportedDaemon, TheCommandsRefusedBelowTheFloorRunAtOrAboveIt) {
  auto server = server_for(*fixture_);
  const auto version = server.tmux_version();
  ASSERT_TRUE(version.has_value());
  ASSERT_TRUE(libtmux::is_supported(*version))
      << "the supported lanes run these against tmux 3.3a or newer";
  const auto created = server.run({"new-session", "-d", "-s", "extra"});
  EXPECT_TRUE(created.has_value()) << (created ? "" : created.error().diagnostic);
  EXPECT_NE(sessions_held_by(*fixture_).find("extra"), std::string::npos);
  EXPECT_TRUE(server.sessions().has_value());
  EXPECT_TRUE(server.check_alive().has_value());
}

TEST_F(SupportedDaemon, RawClientFlagsAndEmptyCommandsAreNotDispatched) {
  auto server = server_for(*fixture_);
  const auto shell = server.run({"-c", "touch should-not-exist"});
  ASSERT_FALSE(shell.has_value());
  EXPECT_EQ(shell.error().kind, FailureKind::validation);
  EXPECT_EQ(shell.error().delivery, DeliveryStatus::not_started);
  const auto control = server.run({"-C", "list-sessions"});
  ASSERT_FALSE(control.has_value());
  EXPECT_EQ(control.error().kind, FailureKind::validation);
  const auto empty = server.run(libtmux::CommandRequest{});
  ASSERT_FALSE(empty.has_value());
  EXPECT_EQ(empty.error().kind, FailureKind::validation);
  // The executable's diagnostics remain available.
  EXPECT_TRUE(server.run({"-V"}).has_value());
}

TEST_F(SupportedDaemon, AdmittingAnEndpointFiresNeitherAfterNorErrorHooks) {
  PrivateDirectory scratch;
  ASSERT_FALSE(scratch.path().empty());
  const auto after = scratch.path() / "after-display-message";
  const auto errors = scratch.path() / "command-error";

  // Set the hooks through one handle, then admit through a fresh one: its
  // first command is the probe.
  auto setter = server_for(*fixture_);
  ASSERT_TRUE(setter
                  .run({"set-hook", "-g", "after-display-message",
                        "run-shell 'echo hit >> " + after.string() + "'"})
                  .has_value());
  // `command-error` exists from tmux 3.5; an older tmux refuses the name.
  const bool has_error_hook =
      setter
          .run({"set-hook", "-g", "command-error",
                "run-shell 'echo hit >> " + errors.string() + "'"})
          .has_value();

  auto fresh = server_for(*fixture_);
  ASSERT_TRUE(fresh.run({"list-sessions"}).has_value());
  ASSERT_TRUE(fresh.tmux_version().has_value());
  ASSERT_TRUE(fresh.run({"list-windows", "-a"}).has_value());

  // Controls: each hook does fire for the thing it names, so silence above is
  // the probe's doing and not a hook that never worked.
  ASSERT_TRUE(setter.run({"display-message", "-p", "control"}).has_value());
  ASSERT_TRUE(wait_for_lines(after, 1U));
  EXPECT_EQ(lines_in(after), 1U) << "admission must not fire after-display-message";
  if (has_error_hook) {
    // With the hook installed tmux answers a failing command with status 0,
    // so the outcome is not asserted; the hook firing is.
    static_cast<void>(setter.run({"kill-session", "-t", "=no-such-session"}));
    ASSERT_TRUE(wait_for_lines(errors, 1U));
    EXPECT_EQ(lines_in(errors), 1U) << "admission must not fire command-error";
  }
}

// ---------------------------------------------------------------------------
// The fence and the live-endpoint flag, against a tmux that is wrapped so the
// arguments the library really sent can be inspected and altered.
// ---------------------------------------------------------------------------

class WrappedTmux : public SupportedDaemon {
protected:
  void SetUp() override {
    SupportedDaemon::SetUp();
    if (HasFatalFailure()) {
      return;
    }
    real_ = find_on_path("tmux");
    ASSERT_FALSE(real_.empty());
    ASSERT_FALSE(scratch_.path().empty());
    log_ = scratch_.path() / "argv.log";
    wrapper_ = scratch_.path() / "tmux-wrapper";
    // Every invocation is logged one argument per line, a blank line apart. A
    // rewrite of the fence's condition stands for a daemon that has been
    // replaced by another release.
    std::ofstream script{wrapper_};
    script << "#!/bin/sh\n"
              "{ for a in \"$@\"; do printf '%s\\n' \"$a\"; done; echo --; } >> \""
           << log_.string()
           << "\"\n"
              "if [ -n \"$LIBTMUX_TEST_REWRITE_FENCE\" ]; then\n"
              "  n=$#; i=0\n"
              "  while [ $i -lt $n ]; do\n"
              "    a=$1; shift\n"
              "    case $a in '#{==:#{version},'*) a='#{==:#{version},9.9z}' ;; esac\n"
              "    set -- \"$@\" \"$a\"\n"
              "    i=$((i+1))\n"
              "  done\n"
              "fi\n"
              "exec \""
           << real_.string() << "\" \"$@\"\n";
    script.close();
    ::chmod(wrapper_.c_str(), 0700);
  }

  [[nodiscard]] ExecutionPolicy wrapped_policy() const {
    ExecutionPolicy policy;
    policy.tmux_binary = wrapper_;
    return policy;
  }

  PrivateDirectory scratch_;
  std::filesystem::path real_;
  std::filesystem::path log_;
  std::filesystem::path wrapper_;
};

TEST_F(WrappedTmux, ALiveEndpointIsFencedAndNeverRestarted) {
  auto server = server_for(*fixture_, wrapped_policy());
  ASSERT_TRUE(server.run({"list-sessions"}).has_value());
  const std::string log = slurp(log_);
  // The probe: no daemon is started to answer it.
  EXPECT_NE(log.find("-N\nrun-shell\n-C\nlibtmux-version-"), std::string::npos) << log;
  // The command: not restartable, and behind a fence naming the admitted
  // release and a marker no command has.
  EXPECT_NE(log.find("-N\nif-shell\n-F\n#{==:#{version},"), std::string::npos) << log;
  EXPECT_NE(log.find("\n;\nlist-sessions\n"), std::string::npos) << log;
}

TEST_F(WrappedTmux, ADaemonThatChangesAfterAdmissionStopsTheCommandListUnrun) {
  auto server = server_for(*fixture_, wrapped_policy());
  ASSERT_TRUE(server.run({"list-sessions"}).has_value());

  const libtmux::test::EnvironmentGuard replaced{"LIBTMUX_TEST_REWRITE_FENCE", "1"};
  const auto refused = server.run({"new-session", "-d", "-s", "intruder"});
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().kind, FailureKind::unsupported)
      << refused.error().diagnostic;
  EXPECT_EQ(refused.error().delivery, DeliveryStatus::not_started);
  EXPECT_NE(refused.error().diagnostic.find("changed"), std::string::npos)
      << refused.error().diagnostic;
  EXPECT_EQ(sessions_held_by(*fixture_), "sentinel\n");

  // A batch is one fenced command list: nothing in it runs.
  libtmux::CommandBatch batch;
  ASSERT_TRUE(batch.add(libtmux::CommandRequest{"new-session", "-d", "-s", "one"}));
  ASSERT_TRUE(batch.add(libtmux::CommandRequest{"new-session", "-d", "-s", "two"}));
  EXPECT_FALSE(server.run_batch(batch).has_value());
  EXPECT_EQ(sessions_held_by(*fixture_), "sentinel\n");
}

std::size_t probes_in(const std::filesystem::path& log) {
  const std::string text = slurp(log);
  std::size_t count = 0;
  for (std::size_t at = text.find("run-shell\n-C\n"); at != std::string::npos;
       at = text.find("run-shell\n-C\n", at + 1U)) {
    ++count;
  }
  return count;
}

TEST_F(WrappedTmux, TheNextCommandAdmitsAgainAfterAChangedDaemonWasRefused) {
  auto server = server_for(*fixture_, wrapped_policy());
  ASSERT_TRUE(server.run({"list-sessions"}).has_value());
  {
    const libtmux::test::EnvironmentGuard replaced{"LIBTMUX_TEST_REWRITE_FENCE", "1"};
    ASSERT_FALSE(server.run({"list-sessions"}).has_value());
  }
  const std::size_t probes_before = probes_in(log_);
  EXPECT_TRUE(server.run({"new-session", "-d", "-s", "after"}).has_value());
  EXPECT_GT(probes_in(log_), probes_before)
      << "the refusal cleared what was remembered, so a probe came before the retry";
  EXPECT_NE(sessions_held_by(*fixture_).find("after"), std::string::npos);
}

TEST_F(WrappedTmux, AbsentEndpointCleanupIsANoOpAndNothingIsStarted) {
  auto server = server_for(*fixture_, wrapped_policy());
  ASSERT_TRUE(server.kill().has_value());
  for (int attempt = 0; attempt < 100 && fixture_->is_alive(); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
  }
  ASSERT_FALSE(fixture_->is_alive());

  // The daemon is gone. Cleanup that finds it gone is not a failure, and no
  // command from this handle restarts it with whatever executable it holds.
  EXPECT_TRUE(server.kill().has_value());
  const std::size_t probes_before = probes_in(log_);
  const auto revived = server.run({"new-session", "-d", "-s", "revived"});
  EXPECT_FALSE(revived.has_value());
  EXPECT_GT(probes_in(log_), probes_before) << "a vanished daemon is asked about again";
  EXPECT_TRUE(server.kill().has_value())
      << "cleanup of an absent endpoint stays a no-op";
  EXPECT_NE(slurp(log_).find("-N\nnew-session\n-d\n-s\nrevived\n"), std::string::npos);
  EXPECT_EQ(sessions_held_by(*fixture_).find("revived"), std::string::npos);
}

TEST_F(WrappedTmux, ANamedExecutableIsAdmittedByTheNameItPrints) {
  // tmux 3.3a prints its own executable name where later releases print tmux.
  const auto renamed = scratch_.path() / "tmux-renamed";
  std::filesystem::copy_file(real_, renamed);
  ::chmod(renamed.c_str(), 0700);

  ExecutionPolicy policy;
  policy.tmux_binary = renamed;
  const auto socket = scratch_.path() / "fresh.sock";
  auto server =
      Server::startable_at_socket_path(socket.string(), std::nullopt, {}, policy);
  ASSERT_TRUE(server.has_value());
  const auto started = server->run({"new-session", "-d", "-s", "fresh"});
  EXPECT_TRUE(started.has_value()) << (started ? "" : started.error().diagnostic);
  EXPECT_TRUE(server->kill().has_value());
}

// ---------------------------------------------------------------------------
// A caller's executor.
// ---------------------------------------------------------------------------

class CountingExecutor final : public libtmux::CommandExecutor {
public:
  explicit CountingExecutor(std::string daemon_version)
      : daemon_version_{std::move(daemon_version)} {}

  libtmux::expected<std::string, CommandFailure>
  run(const libtmux::CommandRequest& command, std::optional<std::chrono::milliseconds>,
      std::optional<std::size_t>) const override {
    const auto argv = command.argv();
    if (argv.size() == 3U && argv[0] == "run-shell" && argv[1] == "-C") {
      ++probes;
      return libtmux::unexpected(CommandFailure{
          .kind = FailureKind::refused,
          .delivery = DeliveryStatus::replied,
          .exit_code = 1,
          .diagnostic = "unknown command: " + argv[2].substr(0U, argv[2].find('#')) +
                        daemon_version_});
    }
    if (replaced && argv.size() > 4U && argv[0] == "if-shell") {
      // The daemon behind the executor is no longer the one that was asked:
      // its fence stops the command list at the marker.
      return libtmux::unexpected(
          CommandFailure{.kind = FailureKind::refused,
                         .delivery = DeliveryStatus::replied,
                         .exit_code = 1,
                         .diagnostic = "unknown command: " + argv[4]});
    }
    ++commands;
    return std::string{"ran\n"};
  }

  mutable int probes{};
  mutable int commands{};
  mutable bool replaced{false};

private:
  std::string daemon_version_;
};

TEST(AdmissionExecutor, ADeclaredVersionBelowTheFloorIsRefusedWithoutCallingIt) {
  auto executor = std::make_shared<CountingExecutor>("3.4");
  auto server =
      Server::over(executor, {.implementation = libtmux::ServerImplementation::tmux,
                              .version = parsed("3.2a")});
  ASSERT_TRUE(server.has_value());
  const auto refused = server->run({"new-session", "-d"});
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().kind, FailureKind::unsupported);
  EXPECT_EQ(executor->commands, 0);
  EXPECT_EQ(executor->probes, 0);
}

TEST(AdmissionExecutor, AnUndeclaredVersionIsAskedOfTheDaemonBehindIt) {
  {
    auto executor = std::make_shared<CountingExecutor>("3.3");
    auto server =
        Server::over(executor, {.implementation = libtmux::ServerImplementation::tmux});
    ASSERT_TRUE(server.has_value());
    EXPECT_FALSE(server->run({"new-session", "-d"}).has_value());
    EXPECT_EQ(executor->probes, 1);
    EXPECT_EQ(executor->commands, 0);
  }
  {
    auto executor = std::make_shared<CountingExecutor>("3.3a");
    auto server =
        Server::over(executor, {.implementation = libtmux::ServerImplementation::tmux});
    ASSERT_TRUE(server.has_value());
    EXPECT_TRUE(server->run({"new-session", "-d"}).has_value());
    EXPECT_TRUE(server->run({"list-sessions"}).has_value());
    EXPECT_EQ(executor->probes, 1) << "a qualified daemon is asked once";
    EXPECT_EQ(executor->commands, 2);
  }
}

TEST(AdmissionExecutor, AReplacedDaemonIsNotAdmittedByTheExecutorsEarlierAnswer) {
  auto executor = std::make_shared<CountingExecutor>("3.4");
  auto server =
      Server::over(executor, {.implementation = libtmux::ServerImplementation::tmux});
  ASSERT_TRUE(server.has_value());
  ASSERT_TRUE(server->run({"list-sessions"}).has_value());
  ASSERT_EQ(executor->probes, 1);

  executor->replaced = true;
  const auto refused = server->run({"new-session", "-d"});
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().kind, FailureKind::unsupported);
  EXPECT_EQ(refused.error().delivery, DeliveryStatus::not_started);
  EXPECT_NE(refused.error().diagnostic.find("changed"), std::string::npos);
  EXPECT_EQ(executor->commands, 1) << "the replaced daemon ran nothing";

  executor->replaced = false;
  EXPECT_TRUE(server->run({"list-sessions"}).has_value());
  EXPECT_EQ(executor->probes, 2) << "the next command asks the new daemon again";
}

TEST(AdmissionExecutor, RawClientFlagsAreNotDispatchedThroughAnExecutor) {
  auto executor = std::make_shared<CountingExecutor>("3.4");
  auto server =
      Server::over(executor, {.implementation = libtmux::ServerImplementation::tmux});
  ASSERT_TRUE(server.has_value());
  const auto shell = server->run({"-c", "touch nope"});
  ASSERT_FALSE(shell.has_value());
  EXPECT_EQ(shell.error().kind, FailureKind::validation);
  EXPECT_FALSE(server->run(libtmux::CommandRequest{}).has_value());
  EXPECT_EQ(executor->commands, 0);
  EXPECT_EQ(executor->probes, 0);
}

TEST(AdmissionExecutor, ReleasesAndDevelopmentBuildsAreAdmittedOrRefusedByRank) {
  // The canonical grammar: every form tmux has reported, and the forms only
  // library-side wrappers invented.
  const std::vector<std::pair<std::string, bool>> reports{{"3.3a", true},
                                                          {"3.4", true},
                                                          {"3.7d", true},
                                                          {"3.8", true},
                                                          {"3.10", true},
                                                          {"4.0", true},
                                                          {"3.4-rc", true},
                                                          {"3.8-rc", true},
                                                          {"3.8-rc2", true},
                                                          {"next-3.4", true},
                                                          {"next-3.9", true},
                                                          {"3.3", false},
                                                          {"3.2a", false},
                                                          {"3.3-rc", false},
                                                          {"3.2-rc5", false},
                                                          {"next-3.3", false},
                                                          {"next-3.3a", false},
                                                          {"master", false},
                                                          {"openbsd-7.6", false},
                                                          {"", false},
                                                          {"3", false},
                                                          {"garbage", false},
                                                          {"3.3a garbage", false},
                                                          {"3.3a-master", false},
                                                          {"3.3a-dev", false},
                                                          {"next-3.9-master", false}};
  for (const auto& [report, admitted] : reports) {
    SCOPED_TRACE(report);
    auto executor = std::make_shared<CountingExecutor>(report);
    auto server =
        Server::over(executor, {.implementation = libtmux::ServerImplementation::tmux});
    ASSERT_TRUE(server.has_value());
    EXPECT_EQ(server->run({"list-sessions"}).has_value(), admitted);
    EXPECT_EQ(executor->commands, admitted ? 1 : 0);
  }
}

// ---------------------------------------------------------------------------
// The asynchronous runtime.
// ---------------------------------------------------------------------------

class Resume final {
public:
  explicit Resume(pid_t process) : process_{process} {}
  ~Resume() { static_cast<void>(::kill(process_, SIGCONT)); }
  Resume(const Resume&) = delete;
  Resume& operator=(const Resume&) = delete;

private:
  pid_t process_;
};

TEST_F(SupportedDaemon, AHungDaemonDoesNotStallOtherSubmitters) {
  using namespace std::chrono_literals;
  auto hung_fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(hung_fixture.has_value()) << hung_fixture.error();
  const Server hung = server_for(*hung_fixture);
  const Server healthy = server_for(*fixture_);
  ASSERT_TRUE(healthy.run({"list-sessions"}).has_value());

  auto runtime = libtmux::CommandRuntime::start();
  ASSERT_TRUE(runtime.has_value()) << runtime.error().diagnostic;
  const Resume resume{hung_fixture->server_pid()};
  ASSERT_EQ(::kill(hung_fixture->server_pid(), SIGSTOP), 0);

  // The first submission to a daemon that never answers waits for it, for as
  // long as its own deadline allows and no longer.
  std::optional<libtmux::expected<libtmux::CommandOperation, CommandFailure>> blocked;
  std::thread submitter{
      [&] { blocked.emplace(hung.try_submit(*runtime, {"list-sessions"}, 1500ms)); }};
  std::this_thread::sleep_for(300ms);

  const auto started = std::chrono::steady_clock::now();
  auto operation = healthy.try_submit(*runtime, {"list-sessions"}, 5s);
  const auto took = std::chrono::steady_clock::now() - started;
  ASSERT_TRUE(operation.has_value()) << operation.error().diagnostic;
  EXPECT_LT(took, 500ms) << "another submitter waited for the hung daemon's probe";
  EXPECT_TRUE(std::move(*operation).wait().has_value());

  submitter.join();
  ASSERT_TRUE(blocked.has_value());
  ASSERT_FALSE(blocked->has_value());
  EXPECT_EQ(blocked->error().kind, FailureKind::timeout);
  EXPECT_EQ(blocked->error().delivery, DeliveryStatus::not_started);
}

TEST_F(SupportedDaemon, AnAsynchronousKillOfAVanishedDaemonIsANoOp) {
  using namespace std::chrono_literals;
  const Server server = server_for(*fixture_);
  ASSERT_TRUE(server.run({"list-sessions"}).has_value());
  ASSERT_TRUE(server.kill().has_value());
  for (int attempt = 0; attempt < 100 && fixture_->is_alive(); ++attempt) {
    std::this_thread::sleep_for(20ms);
  }
  ASSERT_FALSE(fixture_->is_alive());

  auto runtime = libtmux::CommandRuntime::start();
  ASSERT_TRUE(runtime.has_value()) << runtime.error().diagnostic;
  auto operation = server.try_submit(*runtime, {"kill-server"}, 5s);
  ASSERT_TRUE(operation.has_value()) << operation.error().diagnostic;
  const auto answer = std::move(*operation).wait();
  EXPECT_TRUE(answer.has_value()) << (answer ? "" : answer.error().diagnostic);
}

} // namespace
