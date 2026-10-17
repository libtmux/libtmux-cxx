// What a text wait promises when the pane misbehaves.
//
// Wrapped output is found on the polling path and on the control path. A pane
// that dies, or a server that is killed, ends the wait as a failed one that
// keeps the output so far, whichever path was waiting. A control stream that
// drops while the pane lives is not a death: the wait keeps going by polling.
// Each test runs on whichever tmux is on PATH, so the polling path is the one
// below 3.7 and the control path is the one from 3.7.

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "libtmux/entities.hpp"
#include "libtmux/server.hpp"
#include "libtmux/testing/scoped_server.hpp"
#include "libtmux/version.hpp"
#include "libtmux/wait.hpp"

namespace {

using libtmux::Server;
using libtmux::WaitPath;
using libtmux::WaitResult;
using namespace std::chrono_literals;

constexpr libtmux::Version k37{.major = 3, .minor = 7};

struct Lane {
  libtmux::test::ScopedTmuxServer fixture;
  Server server;
  libtmux::Version running;
};

Lane start_lane() {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  EXPECT_TRUE(fixture.has_value()) << (fixture ? "" : fixture.error());
  auto server = Server::at_socket_path(fixture->socket_path().string());
  EXPECT_TRUE(server.has_value());
  const auto version = server->tmux_version();
  EXPECT_TRUE(version.has_value());
  return Lane{std::move(*fixture), *std::move(server), *version};
}

// The wait reports progress from its first pass, so a flag it sets says the
// wait is running; a fixed sleep would race a slow runner. Then the pane has
// had a moment to print.
void wait_until(const std::atomic<bool>& began) {
  for (int attempt = 0; attempt < 200 && !began.load(); ++attempt) {
    std::this_thread::sleep_for(25ms);
  }
  std::this_thread::sleep_for(400ms);
}

// A second pane whose shell command prints `before-death` and then idles.
libtmux::Pane idle_pane(const Server& server) {
  const auto made = server.run({"split-window", "-d", "-P", "-F", "#{pane_id}", "sh",
                                "-c", "echo before-death; sleep 60"});
  EXPECT_TRUE(made.has_value());
  std::string id = made.value_or("");
  while (!id.empty() && (id.back() == '\n' || id.back() == '\r')) {
    id.pop_back();
  }
  auto pane = server.pane(id);
  EXPECT_TRUE(pane.has_value());
  return *pane;
}

// A second `tmux -C attach` client, stopped. tmux keeps its server up until
// every client has left, so a stopped one holds a killed server open: an answer
// that waits for the server to be gone, or for a grace period, would not come.
class StoppedClient {
public:
  StoppedClient(const std::filesystem::path& socket, std::string_view session) {
    int input[2];
    if (::pipe(input) != 0) {
      return;
    }
    pid_ = ::fork();
    if (pid_ == 0) {
      ::dup2(input[0], STDIN_FILENO);
      const int quiet = ::open("/dev/null", O_WRONLY);
      ::dup2(quiet, STDOUT_FILENO);
      ::dup2(quiet, STDERR_FILENO);
      ::close(input[0]);
      ::close(input[1]);
      const std::string target{session};
      ::execlp("tmux", "tmux", "-S", socket.c_str(), "-C", "attach", "-t",
               target.c_str(), static_cast<char*>(nullptr));
      ::_exit(127);
    }
    ::close(input[0]);
    input_ = input[1];
  }
  StoppedClient(const StoppedClient&) = delete;
  StoppedClient& operator=(const StoppedClient&) = delete;
  ~StoppedClient() {
    if (pid_ > 0) {
      ::kill(pid_, SIGKILL);
      ::waitpid(pid_, nullptr, 0);
    }
    if (input_ >= 0) {
      ::close(input_);
    }
  }
  [[nodiscard]] bool started() const noexcept { return pid_ > 0; }
  void stop() const { ::kill(pid_, SIGSTOP); }

private:
  pid_t pid_{-1};
  int input_{-1};
};

TEST(WaitContracts, AMatchSplitAcrossAWrappedRowIsFound) {
  Lane lane = start_lane();
  auto pane = lane.server.pane(lane.fixture.session_name());
  ASSERT_TRUE(pane.has_value());
  // The pieces are separate in the typed command and joined in its output, and
  // the joined line is wider than the pane.
  const std::string head(60, 'a');
  const std::string tail(60, 'b');
  std::thread typist{[&] {
    std::this_thread::sleep_for(800ms);
    EXPECT_TRUE(pane->send_line("printf '%s%s\\n' " + head + " " + tail).has_value());
  }};
  libtmux::WaitOptions options;
  options.timeout = 8s;
  const auto waited = pane->wait_for_text(head + tail, options);
  typist.join();
  ASSERT_TRUE(waited.has_value()) << waited.error().diagnostic;
  EXPECT_TRUE(waited->matched) << "path " << static_cast<int>(waited->path);
  EXPECT_FALSE(waited->matched_at_entry) << "the line printed after the wait began";
  EXPECT_EQ(waited->path,
            lane.running < k37 ? WaitPath::capture_polling : WaitPath::control_output);
}

TEST(WaitContracts, ADeadPaneEndsTheWaitWithTheOutputSoFar) {
  Lane lane = start_lane();
  auto pane = idle_pane(lane.server);
  std::atomic<bool> began{false};
  std::thread killer{[&] {
    wait_until(began);
    EXPECT_TRUE(pane.kill().has_value());
  }};
  libtmux::WaitOptions options;
  options.timeout = 10s;
  options.on_progress = [&began](std::chrono::milliseconds, WaitPath) { began = true; };
  const auto waited = pane.wait_for_text("never-printed", options);
  killer.join();
  ASSERT_TRUE(waited.has_value()) << waited.error().diagnostic;
  EXPECT_TRUE(waited->pane_died);
  EXPECT_FALSE(waited->reason.empty());
  EXPECT_FALSE(waited->matched);
  EXPECT_FALSE(waited->timed_out);
  EXPECT_NE(waited->text.find("before-death"), std::string::npos) << waited->text;
  EXPECT_LT(waited->elapsed, 8s);
}

TEST(WaitContracts, AKilledServerEndsTheWaitTheSameWay) {
  Lane lane = start_lane();
  auto pane = idle_pane(lane.server);
  std::atomic<bool> began{false};
  std::thread killer{[&] {
    wait_until(began);
    static_cast<void>(lane.server.run({"kill-server"}));
  }};
  libtmux::WaitOptions options;
  options.timeout = 10s;
  options.on_progress = [&began](std::chrono::milliseconds, WaitPath) { began = true; };
  const auto waited = pane.wait_for_text("never-printed", options);
  killer.join();
  ASSERT_TRUE(waited.has_value()) << waited.error().diagnostic;
  EXPECT_TRUE(waited->pane_died);
  EXPECT_FALSE(waited->reason.empty());
  EXPECT_FALSE(waited->matched);
  EXPECT_FALSE(waited->timed_out);
  EXPECT_NE(waited->text.find("before-death"), std::string::npos) << waited->text;
  EXPECT_LT(waited->elapsed, 8s);
}

TEST(WaitContracts, AKilledServerEndsTheWaitWhileAnotherClientIsStopped) {
  Lane lane = start_lane();
  StoppedClient other{lane.fixture.socket_path(), lane.fixture.session_name()};
  ASSERT_TRUE(other.started());
  // Attached means listed; the client is stopped only once tmux has seen it.
  for (int attempt = 0; attempt < 200; ++attempt) {
    const auto clients = lane.server.clients();
    if (clients.has_value() && !clients->empty()) {
      break;
    }
    std::this_thread::sleep_for(25ms);
  }
  ASSERT_FALSE(lane.server.clients()->empty()) << "the second client never attached";
  other.stop();

  auto pane = idle_pane(lane.server);
  std::atomic<bool> began{false};
  std::thread killer{[&] {
    wait_until(began);
    static_cast<void>(lane.server.run({"kill-server"}));
  }};
  libtmux::WaitOptions options;
  options.timeout = 10s;
  options.on_progress = [&began](std::chrono::milliseconds, WaitPath) { began = true; };
  const auto waited = pane.wait_for_text("never-printed", options);
  killer.join();
  ASSERT_TRUE(waited.has_value()) << waited.error().diagnostic;
  EXPECT_TRUE(waited->pane_died);
  EXPECT_FALSE(waited->reason.empty());
  EXPECT_FALSE(waited->matched);
  EXPECT_FALSE(waited->timed_out);
  EXPECT_NE(waited->text.find("before-death"), std::string::npos) << waited->text;
  EXPECT_LT(waited->elapsed, 8s);
}

TEST(WaitContracts, ADroppedStreamOnALivePaneKeepsWaitingByPolling) {
  Lane lane = start_lane();
  if (lane.running < k37) {
    GTEST_SKIP() << "library-managed control mode starts at tmux 3.7";
  }
  auto pane = lane.server.pane(lane.fixture.session_name());
  ASSERT_TRUE(pane.has_value());
  ASSERT_TRUE(pane->send_line("sleep 3; echo after-drop-$((40+2))").has_value());

  std::atomic<long> client{0};
  libtmux::WaitOptions options;
  options.timeout = 10s;
  options.observe_control_client = [&client](std::int64_t pid) {
    client = static_cast<long>(pid);
    return std::shared_ptr<void>{};
  };
  std::thread dropper{[&] {
    for (int attempt = 0; attempt < 100 && client.load() == 0; ++attempt) {
      std::this_thread::sleep_for(50ms);
    }
    std::this_thread::sleep_for(300ms);
    if (client.load() != 0) {
      ::kill(static_cast<pid_t>(client.load()), SIGKILL);
    }
  }};
  const auto waited = pane->wait_for_text("after-drop-42", options);
  dropper.join();
  ASSERT_TRUE(waited.has_value()) << waited.error().diagnostic;
  ASSERT_NE(client.load(), 0) << "the wait never opened a control client; path "
                              << static_cast<int>(waited->path) << " matched "
                              << waited->matched;
  EXPECT_FALSE(waited->pane_died) << "the pane lives; only its stream dropped";
  EXPECT_TRUE(waited->matched);
  EXPECT_EQ(waited->path, WaitPath::capture_polling);
}

} // namespace
