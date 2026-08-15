#include "support/scoped_tmux_server.hpp"

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <sys/types.h>
#if defined(__linux__)
#include <sys/ptrace.h>
#endif
#include <sys/wait.h>
#include <unistd.h>

namespace {

class ScopedEnvironment final {
public:
  ScopedEnvironment(std::string name, std::optional<std::string> value)
      : name_(std::move(name)) {
    const auto* existing = std::getenv(name_.c_str());
    if (existing != nullptr) {
      previous_ = existing;
    }
    if (value.has_value()) {
      if (::setenv(name_.c_str(), value->c_str(), 1) != 0) {
        std::abort();
      }
    } else if (::unsetenv(name_.c_str()) != 0) {
      std::abort();
    }
  }

  ~ScopedEnvironment() {
    if (previous_.has_value()) {
      static_cast<void>(::setenv(name_.c_str(), previous_->c_str(), 1));
    } else {
      static_cast<void>(::unsetenv(name_.c_str()));
    }
  }

  ScopedEnvironment(const ScopedEnvironment&) = delete;
  ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

private:
  std::string name_;
  std::optional<std::string> previous_;
};

class ScopedCurrentDirectory final {
public:
  explicit ScopedCurrentDirectory(const std::filesystem::path& path)
      : previous_(std::filesystem::current_path()) {
    std::filesystem::current_path(path);
  }

  ~ScopedCurrentDirectory() {
    std::error_code error;
    std::filesystem::current_path(previous_, error);
  }

  void set(const std::filesystem::path& path) { std::filesystem::current_path(path); }

  ScopedCurrentDirectory(const ScopedCurrentDirectory&) = delete;
  ScopedCurrentDirectory& operator=(const ScopedCurrentDirectory&) = delete;

private:
  std::filesystem::path previous_;
};

std::filesystem::path unique_test_directory(std::string_view stem) {
  static std::atomic<unsigned long> sequence{0};
  auto path = std::filesystem::temp_directory_path() /
              (std::string{stem} + "-" + std::to_string(::getpid()) + "-" +
               std::to_string(sequence.fetch_add(1)));
  std::filesystem::create_directories(path);
  return path;
}

std::vector<std::string> read_lines(const std::filesystem::path& path) {
  std::ifstream input{path};
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(input, line)) {
    lines.push_back(line);
  }
  return lines;
}

// Wait for a line to reach the trace, rather than reading once and hoping.
//
// The trace is appended by the fake tmux the teardown launches, so the line
// appears when that process is scheduled — which on a loaded machine is not
// within the microsecond after the call that spawned it returns. Reading once
// turns "not yet" into "never happened".
bool trace_gains_line(const std::filesystem::path& trace, std::string_view needle,
                      std::chrono::steady_clock::time_point deadline) {
  for (;;) {
    for (const auto& line : read_lines(trace)) {
      if (line.find(needle) != std::string::npos) {
        return true;
      }
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
}

// Waits, for the reason given above `trace_gains_line`: the process writes its
// line when it gets round to it, and reading once turns "not yet" into "never
// happened". Every caller wants the pid once it is known, and a busy runner is
// the case that finds this.
pid_t traced_pid(const std::filesystem::path& trace, std::string_view role,
                 std::chrono::milliseconds patience = std::chrono::seconds{5}) {
  const auto prefix = std::string{role} + "\t";
  const auto deadline = std::chrono::steady_clock::now() + patience;
  for (;;) {
    for (const auto& line : read_lines(trace)) {
      if (!line.starts_with(prefix)) {
        continue;
      }
      const auto position = line.find("PID=");
      if (position != std::string::npos) {
        return static_cast<pid_t>(std::stol(line.substr(position + 4U)));
      }
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return -1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
}

bool report_contains(const std::shared_ptr<libtmux::test::TeardownReport>& report,
                     std::string_view text) {
  for (const auto& message : report->messages) {
    if (message.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool wait_until_not_a_child(pid_t pid, std::chrono::steady_clock::time_point deadline) {
  while (std::chrono::steady_clock::now() < deadline) {
    siginfo_t information{};
    errno = 0;
    const auto result = ::waitid(P_PID, static_cast<id_t>(pid), &information,
                                 WEXITED | WNOHANG | WNOWAIT);
    if (result < 0 && errno == ECHILD) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  return false;
}

bool child_exits_before(pid_t pid, std::chrono::steady_clock::time_point deadline) {
  while (std::chrono::steady_clock::now() < deadline) {
    const auto result = ::waitpid(pid, nullptr, WNOHANG);
    if (result == pid || (result < 0 && errno == ECHILD)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  return false;
}

#if defined(__linux__)
struct DelayedTracer {
  pid_t pid;
  int attach_result;
};

DelayedTracer start_delayed_tracer(pid_t target) {
  std::array<int, 2> status_pipe{-1, -1};
  if (::pipe(status_pipe.data()) != 0) {
    return {.pid = -1, .attach_result = -errno};
  }
  const auto tracer = ::fork();
  if (tracer == 0) {
    static_cast<void>(::close(status_pipe[0]));
    const auto result = ::ptrace(PTRACE_SEIZE, target, nullptr, nullptr);
    const auto reported = result == 0 ? 0 : -errno;
    static_cast<void>(::write(status_pipe[1], &reported, sizeof(reported)));
    static_cast<void>(::close(status_pipe[1]));
    if (reported == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds{500});
    }
    std::_Exit(0);
  }
  static_cast<void>(::close(status_pipe[1]));
  int reported = -ECHILD;
  if (tracer > 0) {
    static_cast<void>(::read(status_pipe[0], &reported, sizeof(reported)));
  }
  static_cast<void>(::close(status_pipe[0]));
  return {.pid = tracer, .attach_result = reported};
}
#endif

std::size_t descriptor_count() {
  return static_cast<std::size_t>(
      std::distance(std::filesystem::directory_iterator{"/proc/self/fd"},
                    std::filesystem::directory_iterator{}));
}

void create_server_then_report_primary_failure(
    const std::shared_ptr<libtmux::test::TeardownReport>& report) {
  auto server = libtmux::test::ScopedTmuxServer::start(
      {.tmux_binary = LIBTMUX_FAKE_TMUX_PATH, .teardown_report = report});
  ASSERT_TRUE(server.has_value()) << server.error();
  ADD_FAILURE() << "primary fixture assertion";
}

TEST(ScopedTmuxServerFailure, InvalidBinaryCleansPartialConstructionTree) {
  const auto parent = unique_test_directory("libtmux-invalid-binary");
  ScopedEnvironment temporary_directory{"TMPDIR", parent.string()};

  auto server = libtmux::test::ScopedTmuxServer::start(
      {.tmux_binary = "/definitely/not/a/tmux-binary"});

  ASSERT_FALSE(server.has_value());
  EXPECT_NE(server.error().find("posix_spawnp"), std::string::npos);
  EXPECT_TRUE(std::filesystem::is_empty(parent));
  std::filesystem::remove(parent);
}

TEST(ScopedTmuxServerFailure, QueryFailureReapsDirectChildAndCleansTree) {
  const auto root = unique_test_directory("libtmux-query-failure");
  const auto trace = root / "trace";
  ScopedEnvironment trace_environment{"LIBTMUX_FAKE_TRACE", trace.string()};
  ScopedEnvironment mode_environment{"LIBTMUX_FAKE_MODE", "query-failure"};

  auto server = libtmux::test::ScopedTmuxServer::start(
      {.tmux_binary = LIBTMUX_FAKE_TMUX_PATH,
       .startup_timeout = std::chrono::milliseconds{200},
       .teardown_timeout = std::chrono::milliseconds{200}});

  ASSERT_FALSE(server.has_value());
  const auto pid = traced_pid(trace, "server");
  ASSERT_GT(pid, 0);
  errno = 0;
  EXPECT_EQ(::waitpid(pid, nullptr, WNOHANG), -1);
  EXPECT_EQ(errno, ECHILD);
  std::error_code error;
  std::filesystem::remove_all(root, error);
}

TEST(ScopedTmuxServerFailure, RejectsResolvedPathThatDiffersFromExactSelector) {
  ScopedEnvironment mode_environment{"LIBTMUX_FAKE_MODE", "path-mismatch"};

  auto server = libtmux::test::ScopedTmuxServer::start(
      {.tmux_binary = LIBTMUX_FAKE_TMUX_PATH,
       .mode = libtmux::test::SocketMode::Path,
       .teardown_timeout = std::chrono::milliseconds{200}});

  ASSERT_FALSE(server.has_value());
  EXPECT_NE(server.error().find("exact socket path"), std::string::npos);
}

TEST(ScopedTmuxServerFailure, RejectsNamedSocketResolvedOutsidePrivateTree) {
  ScopedEnvironment mode_environment{"LIBTMUX_FAKE_MODE", "name-path-escape"};

  auto server = libtmux::test::ScopedTmuxServer::start(
      {.tmux_binary = LIBTMUX_FAKE_TMUX_PATH,
       .mode = libtmux::test::SocketMode::Name,
       .teardown_timeout = std::chrono::milliseconds{200}});

  ASSERT_FALSE(server.has_value());
  EXPECT_NE(server.error().find("outside the fixture tree"), std::string::npos);
}

TEST(ScopedTmuxServerFailure, ReboundAfterPidQueryCannotCreateSession) {
  const auto root = unique_test_directory("libtmux-startup-rebind");
  const auto marker = root / "mutation";
  ScopedEnvironment mode_environment{"LIBTMUX_FAKE_MODE", "startup-rebind"};
  ScopedEnvironment marker_environment{"LIBTMUX_FAKE_MUTATION_MARKER", marker.string()};

  const auto unrelated = ::fork();
  ASSERT_GE(unrelated, 0);
  if (unrelated == 0) {
    for (;;) {
      ::pause();
    }
  }
  ScopedEnvironment replacement_environment{"LIBTMUX_FAKE_REBIND_PID",
                                            std::to_string(unrelated)};

  bool startup_succeeded = false;
  std::string startup_error;
  {
    auto server = libtmux::test::ScopedTmuxServer::start(
        {.tmux_binary = LIBTMUX_FAKE_TMUX_PATH,
         .mode = libtmux::test::SocketMode::Name,
         .teardown_timeout = std::chrono::milliseconds{200}});
    startup_succeeded = server.has_value();
    if (!server.has_value()) {
      startup_error = server.error();
    }
  }

  EXPECT_FALSE(startup_succeeded);
  EXPECT_NE(startup_error.find("session creation ownership changed"),
            std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(marker));

  static_cast<void>(::kill(unrelated, SIGKILL));
  while (::waitpid(unrelated, nullptr, 0) < 0 && errno == EINTR) {
  }
  std::error_code error;
  std::filesystem::remove_all(root, error);
}

TEST(ScopedTmuxServerFailure, RelativeTmpdirCannotRedirectCleanupAfterCwdChange) {
  const auto root = unique_test_directory("libtmux-relative-tmpdir");
  const auto original_root = root / "original";
  const auto rebound_root = root / "rebound";
  std::filesystem::create_directories(original_root / "relative-tmp");
  std::filesystem::create_directories(rebound_root);

  std::filesystem::path owned_tree;
  std::filesystem::path unrelated_tree;
  {
    ScopedCurrentDirectory working_directory{original_root};
    ScopedEnvironment temporary_directory{"TMPDIR", "relative-tmp"};
    std::optional<libtmux::test::ScopedTmuxServer> fixture;
    auto started = libtmux::test::ScopedTmuxServer::start(
        {.tmux_binary = LIBTMUX_FAKE_TMUX_PATH,
         .teardown_timeout = std::chrono::milliseconds{200}});
    ASSERT_TRUE(started.has_value()) << started.error();
    fixture.emplace(std::move(*started));

    EXPECT_TRUE(fixture->tmux_tmpdir().is_absolute());
    owned_tree =
        std::filesystem::weakly_canonical(original_root / fixture->tmux_tmpdir());
    const auto relative_tree = owned_tree.lexically_relative(original_root);
    unrelated_tree = rebound_root / relative_tree;
    std::filesystem::create_directories(unrelated_tree);
    std::ofstream{unrelated_tree / "sentinel"} << "unrelated\n";

    working_directory.set(rebound_root);
    fixture.reset();

    EXPECT_FALSE(std::filesystem::exists(owned_tree));
    EXPECT_TRUE(std::filesystem::exists(unrelated_tree / "sentinel"));
  }

  std::error_code error;
  std::filesystem::remove_all(root, error);
}

TEST(ScopedTmuxServerFailure, SelfExitAndStaleSocketAreSafeAtTeardown) {
  const auto root = unique_test_directory("libtmux-self-exit");
  const auto trace = root / "trace";
  ScopedEnvironment trace_environment{"LIBTMUX_FAKE_TRACE", trace.string()};
  ScopedEnvironment mode_environment{"LIBTMUX_FAKE_MODE", "self-exit"};
  std::filesystem::path private_tree;
  std::filesystem::path stale_socket;
  pid_t pid = -1;

  {
    auto server = libtmux::test::ScopedTmuxServer::start(
        {.tmux_binary = LIBTMUX_FAKE_TMUX_PATH,
         .teardown_timeout = std::chrono::milliseconds{200}});
    ASSERT_TRUE(server.has_value()) << server.error();
    private_tree = server->tmux_tmpdir();
    stale_socket = server->socket_path();
    pid = server->server_pid();
    // The fake exits on its own 250ms after it starts. Sleeping 350ms here
    // assumed its clock and this one line up closely enough, and under a
    // sanitizer they do not: it starts slower and exits slower, and the test
    // called it alive when it was only late. Wait for the exit instead.
    const auto gone_by = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (server->is_alive() && std::chrono::steady_clock::now() < gone_by) {
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    EXPECT_FALSE(server->is_alive());
    EXPECT_TRUE(std::filesystem::exists(stale_socket));
  }

  EXPECT_FALSE(std::filesystem::exists(private_tree));
  errno = 0;
  EXPECT_EQ(::waitpid(pid, nullptr, WNOHANG), -1);
  EXPECT_EQ(errno, ECHILD);
  std::error_code error;
  std::filesystem::remove_all(root, error);
}

TEST(ScopedTmuxServerFailure, RejectsOverlongSocketWithoutDeletingTmpParent) {
  auto parent = unique_test_directory("libtmux-long-parent");
  parent /= std::string(90U, 'x');
  std::filesystem::create_directories(parent);
  ScopedEnvironment temporary_directory{"TMPDIR", parent.string()};

  auto server = libtmux::test::ScopedTmuxServer::start(
      {.tmux_binary = LIBTMUX_FAKE_TMUX_PATH, .mode = libtmux::test::SocketMode::Path});

  ASSERT_FALSE(server.has_value());
  EXPECT_NE(server.error().find("socket path"), std::string::npos);
  EXPECT_TRUE(std::filesystem::exists(parent));
  EXPECT_TRUE(std::filesystem::is_empty(parent));
  const auto top = parent.parent_path();
  std::error_code error;
  std::filesystem::remove_all(top, error);
}

TEST(ScopedTmuxServerFailure, TermResistantServerIsKilledAndReapedByDeadline) {
  const auto root = unique_test_directory("libtmux-term-resistant");
  const auto trace = root / "trace";
  ScopedEnvironment trace_environment{"LIBTMUX_FAKE_TRACE", trace.string()};
  ScopedEnvironment mode_environment{"LIBTMUX_FAKE_MODE", "term-resistant"};
  auto report = std::make_shared<libtmux::test::TeardownReport>();
  pid_t pid = -1;

  constexpr auto kTeardown = std::chrono::milliseconds{300};
  std::chrono::steady_clock::time_point torn_down_from{};
  {
    auto server =
        libtmux::test::ScopedTmuxServer::start({.tmux_binary = LIBTMUX_FAKE_TMUX_PATH,
                                                .teardown_timeout = kTeardown,
                                                .teardown_report = report});
    ASSERT_TRUE(server.has_value()) << server.error();
    pid = server->server_pid();
    // Started after the server is up: what is bounded is the reaping, and
    // starting a process is not part of that. Measuring both made this fail
    // under a sanitizer with the rest of the suite alongside it, which says
    // nothing about the deadline being honoured.
    torn_down_from = std::chrono::steady_clock::now();
  }
  const auto elapsed = std::chrono::steady_clock::now() - torn_down_from;

  // Against the deadline the fixture was given rather than a round number.
  // The multiple is headroom for a loaded machine; the claim is that reaping
  // is bounded by the deadline, not that the machine is quick.
  EXPECT_LT(elapsed, kTeardown * 4);
  EXPECT_TRUE(report_contains(report, "SIGTERM"));
  EXPECT_TRUE(report_contains(report, "SIGKILL"));

  // Nothing is left behind. Usually the fixture has already reaped by the time
  // teardown returns, and this says ECHILD at once. When teardown overruns its
  // deadline the child goes to a background reaper instead, and then this is a
  // race the test can lose on a loaded machine — losing it means the call here
  // does the reaping and answers with the pid.
  //
  // Both are acceptable and the claim survives either: after one collection
  // the child is gone. A child nobody reaps still fails, which is the point.
  errno = 0;
  auto reaped = ::waitpid(pid, nullptr, WNOHANG);
  if (reaped == pid) {
    errno = 0;
    reaped = ::waitpid(pid, nullptr, WNOHANG);
  }
  EXPECT_EQ(reaped, -1);
  EXPECT_EQ(errno, ECHILD);
  std::error_code error;
  std::filesystem::remove_all(root, error);
}

#if defined(__linux__)
TEST(ScopedTmuxServerFailure, LatePtraceReapUsesBoundedOwnedHandoff) {
  ScopedEnvironment mode_environment{"LIBTMUX_FAKE_MODE", "ptrace-reap-delay"};
  std::optional<libtmux::test::ScopedTmuxServer> fixture;
  auto started = libtmux::test::ScopedTmuxServer::start(
      {.tmux_binary = LIBTMUX_FAKE_TMUX_PATH,
       .teardown_timeout = std::chrono::milliseconds{80}});
  ASSERT_TRUE(started.has_value()) << started.error();
  fixture.emplace(std::move(*started));
  const auto server_pid = fixture->server_pid();
  const auto tracer = start_delayed_tracer(server_pid);
  ASSERT_GT(tracer.pid, 0);
  ASSERT_EQ(tracer.attach_result, 0);

  const auto teardown_started = std::chrono::steady_clock::now();
  fixture.reset();
  const auto teardown_elapsed = std::chrono::steady_clock::now() - teardown_started;

  EXPECT_LT(teardown_elapsed, std::chrono::milliseconds{300});
  while (::waitpid(tracer.pid, nullptr, 0) < 0 && errno == EINTR) {
  }
  EXPECT_TRUE(wait_until_not_a_child(server_pid, std::chrono::steady_clock::now() +
                                                     std::chrono::seconds{2}));
}
#endif

TEST(ScopedTmuxServerFailure, ReboundSocketCannotKillUnrelatedProcess) {
  const auto root = unique_test_directory("libtmux-rebound-socket");
  const auto trace = root / "trace";
  ScopedEnvironment trace_environment{"LIBTMUX_FAKE_TRACE", trace.string()};
  std::optional<libtmux::test::ScopedTmuxServer> fixture;
  // Long enough for the teardown's tmux to be scheduled on a busy machine.
  // This test asserts what that command was, not how quickly it ran, so a
  // budget that cuts it short only hides the thing being checked.
  auto started = libtmux::test::ScopedTmuxServer::start(
      {.tmux_binary = LIBTMUX_FAKE_TMUX_PATH,
       .teardown_timeout = std::chrono::seconds{5}});
  ASSERT_TRUE(started.has_value()) << started.error();
  fixture.emplace(std::move(*started));
  const auto owned_pid = fixture->server_pid();

  const auto unrelated = ::fork();
  ASSERT_GE(unrelated, 0);
  if (unrelated == 0) {
    for (;;) {
      ::pause();
    }
  }

  auto metadata_path = fixture->socket_path();
  metadata_path += ".meta";
  {
    std::ofstream metadata{metadata_path, std::ios::trunc};
    metadata << unrelated << '\n' << fixture->socket_path().string() << '\n';
  }

  fixture.reset();

  const auto unrelated_was_killed = child_exits_before(
      unrelated, std::chrono::steady_clock::now() + std::chrono::milliseconds{200});
  EXPECT_FALSE(unrelated_was_killed);
  const auto expected_condition =
      "\tif-shell\t-F\t#{==:#{pid}," + std::to_string(owned_pid) + "}\tkill-server\t";
  EXPECT_TRUE(
      trace_gains_line(trace, expected_condition,
                       std::chrono::steady_clock::now() + std::chrono::seconds{5}));
  if (!unrelated_was_killed) {
    static_cast<void>(::kill(unrelated, SIGKILL));
    while (::waitpid(unrelated, nullptr, 0) < 0 && errno == EINTR) {
    }
  }
  std::error_code error;
  std::filesystem::remove_all(root, error);
}

TEST(ScopedTmuxServerFailure, EscapedPipeHolderIsNeitherWaitedForNorSignalled) {
  const auto root = unique_test_directory("libtmux-escaped-holder");
  const auto trace = root / "trace";
  ScopedEnvironment trace_environment{"LIBTMUX_FAKE_TRACE", trace.string()};
  ScopedEnvironment mode_environment{"LIBTMUX_FAKE_MODE", "escaped-holder"};
  pid_t direct_pid = -1;
  pid_t descendant_pid = -1;

  {
    auto server = libtmux::test::ScopedTmuxServer::start(
        {.tmux_binary = LIBTMUX_FAKE_TMUX_PATH,
         .teardown_timeout = std::chrono::milliseconds{250}});
    ASSERT_TRUE(server.has_value()) << server.error();
    direct_pid = server->server_pid();
    descendant_pid = traced_pid(trace, "descendant");
    ASSERT_GT(descendant_pid, 0);
  }

  // The holder outliving teardown is the whole proof, and it is a fact about
  // what teardown did rather than about how long it took: a process that was
  // waited for would be reaped, and one that was signalled would be gone.
  // Timing this instead would only be asking whether the machine was busy.
  errno = 0;
  EXPECT_EQ(::waitpid(direct_pid, nullptr, WNOHANG), -1);
  EXPECT_EQ(errno, ECHILD);
  EXPECT_EQ(::kill(descendant_pid, 0), 0);
  // This test owns it now: nothing else will ever reap a process the fixture
  // deliberately let escape.
  static_cast<void>(::kill(descendant_pid, SIGKILL));
  const auto descendant_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (::kill(descendant_pid, 0) == 0 &&
         std::chrono::steady_clock::now() < descendant_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  std::error_code error;
  std::filesystem::remove_all(root, error);
}

TEST(ScopedTmuxServerFailure, LargeDualPipeOutputCannotDeadlockTeardown) {
  ScopedEnvironment mode_environment{"LIBTMUX_FAKE_MODE", "large-output"};
  const auto started = std::chrono::steady_clock::now();
  {
    auto server = libtmux::test::ScopedTmuxServer::start(
        {.tmux_binary = LIBTMUX_FAKE_TMUX_PATH,
         .teardown_timeout = std::chrono::milliseconds{500}});
    ASSERT_TRUE(server.has_value()) << server.error();
  }
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds{2});
}

TEST(ScopedTmuxServerFailure, RepeatedLifecycleKeepsDescriptorCountStable) {
  const auto before = descriptor_count();
  for (int index = 0; index < 10; ++index) {
    auto server =
        libtmux::test::ScopedTmuxServer::start({.tmux_binary = LIBTMUX_FAKE_TMUX_PATH});
    ASSERT_TRUE(server.has_value()) << server.error();
  }
  EXPECT_EQ(descriptor_count(), before);
}

TEST(ScopedTmuxServerFailure, TeardownDiagnosticsDoNotMaskPrimaryAssertion) {
  auto report = std::make_shared<libtmux::test::TeardownReport>();

  EXPECT_NONFATAL_FAILURE(create_server_then_report_primary_failure(report),
                          "primary fixture assertion");

  EXPECT_FALSE(report->messages.empty());
}

} // namespace
