#include "libtmux/control.hpp"
#include "libtmux/expected.hpp"

#include "support/scoped_tmux_server.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

#ifndef LIBTMUX_CONTROL_TMUX_PATH
#error "LIBTMUX_CONTROL_TMUX_PATH must name the tested tmux executable"
#endif
#ifndef LIBTMUX_CONTROL_TMUX_SHA256
#error "LIBTMUX_CONTROL_TMUX_SHA256 must bind the tested tmux executable"
#endif

namespace {

using namespace std::chrono_literals;
using libtmux::Attribution;
using libtmux::Connection;
using libtmux::ConnectionOptions;
using libtmux::ControlCommand;
using libtmux::ControlRequest;
using libtmux::ControlRequestResult;
using libtmux::ControlTerminal;
using libtmux::Notification;
using libtmux::ProtocolError;
using libtmux::test::ScopedTmuxServer;
using libtmux::test::ScopedTmuxServerOptions;
using libtmux::test::SocketMode;

std::string unique_name(std::string_view prefix) {
  static std::atomic<unsigned int> sequence{0U};
  return std::string{prefix} + "-" + std::to_string(::getpid()) + "-" +
         std::to_string(sequence.fetch_add(1U, std::memory_order_relaxed));
}

libtmux::expected<ScopedTmuxServer, std::string>
start_server(std::string session_name) {
  return ScopedTmuxServer::start({.tmux_binary = LIBTMUX_CONTROL_TMUX_PATH,
                                  .mode = SocketMode::Path,
                                  .startup_timeout = 2s,
                                  .teardown_timeout = 2s,
                                  .session_name = std::move(session_name),
                                  .teardown_report = {}});
}

libtmux::expected<Connection, ProtocolError>
connect_to(const ScopedTmuxServer& server) {
  return Connection::connect({.tmux_binary = LIBTMUX_CONTROL_TMUX_PATH,
                              .socket_path = server.socket_path(),
                              .session_name = std::string{server.session_name()},
                              .startup_timeout = 2s,
                              .shutdown_timeout = 2s});
}

ControlRequest
group(std::initializer_list<std::initializer_list<std::string_view>> operations) {
  ControlRequest request;
  request.group.reserve(operations.size());
  for (const auto operation : operations) {
    ControlCommand command;
    command.argv.reserve(operation.size());
    for (const auto argument : operation) {
      command.argv.emplace_back(argument);
    }
    request.group.push_back(std::move(command));
  }
  return request;
}

std::string text(const std::vector<std::byte>& value) {
  std::string result;
  result.reserve(value.size());
  for (const auto byte : value) {
    result.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
  }
  return result;
}

template <typename Value>
const Value& required_value(const std::optional<Value>& value) {
  if (!value.has_value()) {
    throw std::logic_error{"required test value is missing"};
  }
  return value.value();
}

void expect_exact_end(const ControlRequestResult& result,
                      std::string_view expected_body) {
  ASSERT_FALSE(result.connection_error.has_value())
      << (result.connection_error ? result.connection_error->message : "");
  ASSERT_EQ(result.operations.size(), 1U);
  EXPECT_EQ(result.operations[0].attribution, Attribution::exact);
  ASSERT_TRUE(result.operations[0].block.has_value());
  EXPECT_EQ(required_value(result.operations[0].block).terminal, ControlTerminal::end);
  EXPECT_EQ(text(required_value(result.operations[0].block).body), expected_body);
}

bool has_notification(const std::vector<Notification>& notifications,
                      std::string_view prefix) {
  return std::ranges::any_of(notifications, [prefix](const auto& notification) {
    return text(notification.body).starts_with(prefix);
  });
}

std::optional<int> queued_control_input(pid_t child_pid) {
  std::error_code error;
  const auto child_input = std::filesystem::read_symlink(
      "/proc/" + std::to_string(child_pid) + "/fd/0", error);
  if (error) {
    return std::nullopt;
  }
  for (const auto& entry :
       std::filesystem::directory_iterator{"/proc/self/fd", error}) {
    if (error) {
      return std::nullopt;
    }
    std::error_code link_error;
    const auto input = std::filesystem::read_symlink(entry.path(), link_error);
    if (link_error || input != child_input) {
      continue;
    }
    int queued = 0;
    const auto descriptor = std::stoi(entry.path().filename().string());
    if (::ioctl(descriptor, FIONREAD, &queued) == 0) {
      return queued;
    }
  }
  return std::nullopt;
}

TEST(ControlModeConnection, FailFastGroupMarksDeletedSuffixSkipped) {
  auto server = start_server(unique_name("control-fail-fast"));
  ASSERT_TRUE(server.has_value()) << (server.has_value() ? "" : server.error());
  auto connected = connect_to(*server);
  ASSERT_TRUE(connected.has_value())
      << (connected.has_value() ? "" : connected.error().message);
  auto connection = std::move(*connected);

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  const auto result =
      connection.execute(group({{"kill-session", "-t", "=libtmux-control-missing"},
                                {"display-message", "-p", "second-must-not-run"},
                                {"display-message", "-p", "third-must-not-run"}}),
                         deadline);

  EXPECT_LT(std::chrono::steady_clock::now(), deadline);
  ASSERT_FALSE(result.connection_error.has_value())
      << (result.connection_error ? result.connection_error->message : "");
  ASSERT_EQ(result.operations.size(), 3U);
  EXPECT_EQ(result.operations[0].attribution, Attribution::exact);
  ASSERT_TRUE(result.operations[0].block.has_value());
  EXPECT_EQ(required_value(result.operations[0].block).terminal,
            ControlTerminal::error);
  EXPECT_NE(
      text(required_value(result.operations[0].block).body).find("can't find session"),
      std::string::npos);
  EXPECT_EQ(result.operations[1].attribution, Attribution::skipped);
  EXPECT_FALSE(result.operations[1].block.has_value());
  EXPECT_EQ(result.operations[2].attribution, Attribution::skipped);
  EXPECT_FALSE(result.operations[2].block.has_value());

  const auto next =
      connection.execute(group({{"display-message", "-p", "after-fail-fast"}}),
                         std::chrono::steady_clock::now() + 2s);
  expect_exact_end(next, "after-fail-fast\n");
  EXPECT_TRUE(connection.shutdown(std::chrono::steady_clock::now() + 2s).has_value());
}

TEST(ControlModeConnection, ConcurrentIndependentRequestsKeepReplyOwnership) {
  auto server = start_server(unique_name("control-independent"));
  ASSERT_TRUE(server.has_value()) << (server.has_value() ? "" : server.error());
  auto connected = connect_to(*server);
  ASSERT_TRUE(connected.has_value())
      << (connected.has_value() ? "" : connected.error().message);
  auto connection = std::move(*connected);

  const auto window_name = unique_name("control-window");
  std::optional<ControlRequestResult> first;
  std::optional<ControlRequestResult> second;
  std::barrier ready{3};
  std::thread first_thread{[&] {
    ready.arrive_and_wait();
    first = connection.execute(
        group({{"new-window", "-d", "-P", "-F", "first-marker", "-n", window_name}}),
        std::chrono::steady_clock::now() + 2s);
  }};
  std::thread second_thread{[&] {
    ready.arrive_and_wait();
    second = connection.execute(group({{"display-message", "-p", "second-marker"}}),
                                std::chrono::steady_clock::now() + 2s);
  }};
  ready.arrive_and_wait();
  first_thread.join();
  second_thread.join();

  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  expect_exact_end(required_value(first), "first-marker\n");
  expect_exact_end(required_value(second), "second-marker\n");

  std::vector<Notification> notifications;
  const auto notification_deadline = std::chrono::steady_clock::now() + 1s;
  while (std::chrono::steady_clock::now() < notification_deadline &&
         !has_notification(notifications, "%window-add ")) {
    auto available = connection.take_notifications();
    notifications.insert(notifications.end(),
                         std::make_move_iterator(available.begin()),
                         std::make_move_iterator(available.end()));
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_TRUE(has_notification(notifications, "%window-add "));
  EXPECT_TRUE(connection.shutdown(std::chrono::steady_clock::now() + 2s).has_value());
}

TEST(ControlModeConnection, NotificationShapedCommandOutputRemainsBlockBody) {
  auto server = start_server(unique_name("control-shaped-body"));
  ASSERT_TRUE(server.has_value()) << (server.has_value() ? "" : server.error());
  auto connected = connect_to(*server);
  ASSERT_TRUE(connected.has_value())
      << (connected.has_value() ? "" : connected.error().message);
  auto connection = std::move(*connected);

  const auto result = connection.execute(group({{"display-message", "direct-body"}}),
                                         std::chrono::steady_clock::now() + 2s);
  expect_exact_end(result, "%message direct-body\n");
  const auto notifications = connection.take_notifications();
  EXPECT_FALSE(has_notification(notifications, "%message direct-body"));
  EXPECT_TRUE(connection.shutdown(std::chrono::steady_clock::now() + 2s).has_value());
}

TEST(ControlModeConnection, EncodesArgumentsWithoutCreatingAnotherCommand) {
  auto server = start_server(unique_name("control-encoding"));
  ASSERT_TRUE(server.has_value()) << (server.has_value() ? "" : server.error());
  auto connected = connect_to(*server);
  ASSERT_TRUE(connected.has_value())
      << (connected.has_value() ? "" : connected.error().message);
  auto connection = std::move(*connected);

  std::string high_byte;
  high_byte.push_back(static_cast<char>(0x80));
  const std::vector<std::string> payloads{"",
                                          "'",
                                          "\\",
                                          "line one\nline two",
                                          "literal ; display-message -p injected",
                                          high_byte};
  for (const auto& payload : payloads) {
    SCOPED_TRACE(payload.size());
    ControlRequest request;
    request.group.push_back(
        ControlCommand{.argv = {"display-message", "-p", "--", payload}});
    const auto result =
        connection.execute(std::move(request), std::chrono::steady_clock::now() + 2s);
    expect_exact_end(result, payload + "\n");
  }
  EXPECT_TRUE(connection.shutdown(std::chrono::steady_clock::now() + 2s).has_value());
}

TEST(ControlModeConnection, ShutdownIsBoundedAndLeavesFixtureAlive) {
  auto server = start_server(unique_name("control-shutdown"));
  ASSERT_TRUE(server.has_value()) << (server.has_value() ? "" : server.error());
  auto connected = connect_to(*server);
  ASSERT_TRUE(connected.has_value())
      << (connected.has_value() ? "" : connected.error().message);
  auto connection = std::move(*connected);
  const auto child_pid = connection.native_child_pid();
  ASSERT_GT(child_pid, 0);

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  EXPECT_TRUE(connection.shutdown(deadline).has_value());
  EXPECT_LT(std::chrono::steady_clock::now(), deadline);
  EXPECT_TRUE(server->is_alive());
  int status = 0;
  errno = 0;
  EXPECT_EQ(::waitpid(static_cast<pid_t>(child_pid), &status, WNOHANG), -1);
  EXPECT_EQ(errno, ECHILD);
  EXPECT_TRUE(connection.shutdown(std::chrono::steady_clock::now() + 2s).has_value());
}

TEST(ControlModeConnection, ExecuteVsShutdownCompletesOnceAndMarksUnresolvedUnknown) {
  auto server = start_server(unique_name("control-shutdown-race"));
  ASSERT_TRUE(server.has_value()) << (server.has_value() ? "" : server.error());
  auto connected = connect_to(*server);
  ASSERT_TRUE(connected.has_value())
      << (connected.has_value() ? "" : connected.error().message);
  auto connection = std::move(*connected);

  const auto window_name = unique_name("control-race-window");
  const auto marker = server->tmux_tmpdir() / "request-started";
  const auto shell_command = "touch " + marker.string() + " ; sleep 5";
  std::optional<ControlRequestResult> result;
  std::atomic<unsigned int> completions{0U};
  std::thread request_thread{[&] {
    result = connection.execute(
        group({{"new-window", "-d", "-P", "-F", "race-window", "-n", window_name},
               {"run-shell", shell_command},
               {"display-message", "-p", "must-remain-unresolved"}}),
        std::chrono::steady_clock::now() + 10s);
    completions.fetch_add(1U, std::memory_order_relaxed);
  }};

  std::vector<Notification> notifications;
  const auto marker_deadline = std::chrono::steady_clock::now() + 2s;
  while ((!std::filesystem::exists(marker) ||
          !has_notification(notifications, "%window-add ")) &&
         std::chrono::steady_clock::now() < marker_deadline) {
    auto available = connection.take_notifications();
    notifications.insert(notifications.end(),
                         std::make_move_iterator(available.begin()),
                         std::make_move_iterator(available.end()));
    std::this_thread::sleep_for(1ms);
  }
  const auto marker_seen = std::filesystem::exists(marker);
  const auto notification_seen = has_notification(notifications, "%window-add ");
  const auto shutdown = connection.shutdown(std::chrono::steady_clock::now() + 2s);
  request_thread.join();

  ASSERT_TRUE(marker_seen);
  ASSERT_TRUE(notification_seen);
  ASSERT_TRUE(shutdown.has_value())
      << (shutdown.has_value() ? "" : shutdown.error().message);
  ASSERT_TRUE(result.has_value());
  const auto& completed = required_value(result);
  EXPECT_EQ(completions.load(std::memory_order_relaxed), 1U);
  ASSERT_EQ(completed.operations.size(), 3U);
  EXPECT_EQ(completed.operations[0].attribution, Attribution::exact);
  EXPECT_TRUE(completed.operations[0].block.has_value());
  EXPECT_TRUE(completed.operations[1].attribution == Attribution::exact ||
              completed.operations[1].attribution == Attribution::unknown);
  EXPECT_EQ(completed.operations[1].block.has_value(),
            completed.operations[1].attribution == Attribution::exact);
  EXPECT_EQ(completed.operations[2].attribution, Attribution::unknown);
  EXPECT_FALSE(completed.operations[2].block.has_value());
  EXPECT_TRUE(completed.connection_error.has_value());
  EXPECT_TRUE(server->is_alive());
}

TEST(ControlModeConnection, LargeSubmitVsShutdownIsBoundedAndMarksUnknown) {
  auto server = start_server(unique_name("control-large-shutdown-race"));
  ASSERT_TRUE(server.has_value()) << (server.has_value() ? "" : server.error());
  auto connected = connect_to(*server);
  ASSERT_TRUE(connected.has_value())
      << (connected.has_value() ? "" : connected.error().message);
  auto connection = std::move(*connected);
  const auto child_pid = connection.native_child_pid();
  ASSERT_GT(child_pid, 0);
  ASSERT_EQ(::kill(static_cast<pid_t>(child_pid), SIGSTOP), 0);

  ControlRequest large_request;
  large_request.group.push_back(ControlCommand{
      .argv = {"display-message", "-p", std::string(4U * 1024U * 1024U, 'x')}});
  std::optional<ControlRequestResult> result;
  std::atomic<unsigned int> completions{0U};
  std::barrier started{2};
  std::thread request_thread{[&] {
    started.arrive_and_wait();
    result = connection.execute(std::move(large_request),
                                std::chrono::steady_clock::now() + 10s);
    completions.fetch_add(1U, std::memory_order_relaxed);
  }};
  started.arrive_and_wait();
  std::optional<int> queued;
  const auto partial_deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < partial_deadline) {
    queued = queued_control_input(static_cast<pid_t>(child_pid));
    if (queued && *queued > 0) {
      break;
    }
    std::this_thread::sleep_for(1ms);
  }
  const auto partial_written = queued && *queued > 0;

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  const auto shutdown = connection.shutdown(deadline);
  request_thread.join();

  ASSERT_TRUE(partial_written);
  EXPECT_LT(std::chrono::steady_clock::now(), deadline);
  if (!shutdown.has_value()) {
    EXPECT_EQ(shutdown.error().message, "control client exited without %exit");
  }
  ASSERT_TRUE(result.has_value());
  const auto& completed = required_value(result);
  EXPECT_EQ(completions.load(std::memory_order_relaxed), 1U);
  ASSERT_EQ(completed.operations.size(), 1U);
  EXPECT_EQ(completed.operations[0].attribution, Attribution::unknown);
  EXPECT_FALSE(completed.operations[0].block.has_value());
  EXPECT_TRUE(completed.connection_error.has_value());
  int status = 0;
  errno = 0;
  EXPECT_EQ(::waitpid(static_cast<pid_t>(child_pid), &status, WNOHANG), -1);
  EXPECT_EQ(errno, ECHILD);
  EXPECT_TRUE(server->is_alive());
}

TEST(ControlModeConnection, PartialWriteShutdownNeverDispatchesTruncatedCommand) {
  auto server = start_server(unique_name("control-partial-write"));
  ASSERT_TRUE(server.has_value()) << (server.has_value() ? "" : server.error());
  auto connected = connect_to(*server);
  ASSERT_TRUE(connected.has_value())
      << (connected.has_value() ? "" : connected.error().message);
  auto connection = std::move(*connected);
  const auto child_pid = connection.native_child_pid();
  ASSERT_GT(child_pid, 0);
  ASSERT_EQ(::kill(static_cast<pid_t>(child_pid), SIGSTOP), 0);

  const auto marker = server->tmux_tmpdir() / "truncated-command-ran";
  auto shell_command = "touch " + marker.string() + " ; # ";
  shell_command.append(4U * 1024U * 1024U, 'x');
  ControlRequest request;
  request.group.push_back(
      ControlCommand{.argv = {"run-shell", "-E", "-c", "/", std::move(shell_command)}});
  std::optional<ControlRequestResult> result;
  std::atomic<bool> request_done{false};
  std::barrier started{2};
  std::thread request_thread{[&] {
    started.arrive_and_wait();
    result =
        connection.execute(std::move(request), std::chrono::steady_clock::now() + 10s);
    request_done.store(true, std::memory_order_release);
  }};
  started.arrive_and_wait();
  std::optional<int> queued;
  const auto partial_deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < partial_deadline) {
    queued = queued_control_input(static_cast<pid_t>(child_pid));
    if (queued && *queued > 0) {
      break;
    }
    std::this_thread::sleep_for(1ms);
  }
  const auto partial_written = queued && *queued > 0;

  std::optional<libtmux::expected<void, ProtocolError>> shutdown;
  std::thread shutdown_thread{
      [&] { shutdown = connection.shutdown(std::chrono::steady_clock::now() + 2s); }};
  const auto cancellation_deadline = std::chrono::steady_clock::now() + 500ms;
  while (!request_done.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < cancellation_deadline) {
    std::this_thread::sleep_for(1ms);
  }
  const auto request_cancelled = request_done.load(std::memory_order_acquire);
  const auto resumed = ::kill(static_cast<pid_t>(child_pid), SIGCONT) == 0;
  shutdown_thread.join();
  request_thread.join();

  ASSERT_TRUE(resumed);
  ASSERT_TRUE(partial_written);
  ASSERT_TRUE(request_cancelled);
  ASSERT_TRUE(shutdown.has_value());
  ASSERT_TRUE(result.has_value());
  const auto& completed = required_value(result);
  ASSERT_EQ(completed.operations.size(), 1U);
  EXPECT_EQ(completed.operations[0].attribution, Attribution::unknown);
  EXPECT_FALSE(completed.operations[0].block.has_value());
  EXPECT_TRUE(completed.connection_error.has_value());
  const auto marker_deadline = std::chrono::steady_clock::now() + 1s;
  while (!std::filesystem::exists(marker) &&
         std::chrono::steady_clock::now() < marker_deadline) {
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_FALSE(std::filesystem::exists(marker));
  int status = 0;
  errno = 0;
  EXPECT_EQ(::waitpid(static_cast<pid_t>(child_pid), &status, WNOHANG), -1);
  EXPECT_EQ(errno, ECHILD);
  EXPECT_TRUE(server->is_alive());
}

TEST(ControlModeConnection, ConcurrentShutdownHonorsEachCallerDeadline) {
  auto server = start_server(unique_name("control-concurrent-shutdown"));
  ASSERT_TRUE(server.has_value()) << (server.has_value() ? "" : server.error());
  auto connected = connect_to(*server);
  ASSERT_TRUE(connected.has_value())
      << (connected.has_value() ? "" : connected.error().message);
  auto connection = std::move(*connected);
  const auto child_pid = connection.native_child_pid();
  ASSERT_GT(child_pid, 0);
  ASSERT_EQ(::kill(static_cast<pid_t>(child_pid), SIGSTOP), 0);

  std::optional<libtmux::expected<void, ProtocolError>> first;
  std::barrier started{2};
  std::thread first_thread{[&] {
    started.arrive_and_wait();
    first = connection.shutdown(std::chrono::steady_clock::now() + 2s);
  }};
  started.arrive_and_wait();
  std::this_thread::sleep_for(100ms);

  const auto second_deadline = std::chrono::steady_clock::now() + 200ms;
  const auto second = connection.shutdown(second_deadline);
  const auto second_finished = std::chrono::steady_clock::now();
  first_thread.join();

  EXPECT_LE(second_finished, second_deadline + 100ms);
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().message, "control shutdown acquisition deadline expired");
  ASSERT_TRUE(first.has_value());
  EXPECT_FALSE(required_value(first).has_value());
  int status = 0;
  errno = 0;
  EXPECT_EQ(::waitpid(static_cast<pid_t>(child_pid), &status, WNOHANG), -1);
  EXPECT_EQ(errno, ECHILD);
  EXPECT_TRUE(server->is_alive());
}

// A waiter that times out acquiring the writer must not poison the owner.
//
// The scenario needs the owner to still hold the writer when the waiter
// arrives, which stopping the client and writing four megabytes only makes
// likely: if the owner releases early the waiter dispatches instead, times out
// after dispatch, and measures something else. That interleaving is reported
// rather than asserted against, and the attempt is retried, so the test either
// exercises the contract it names or says it never reached it.
//
// Waiting a fixed interval for the owner to get there is what a busy machine
// breaks: the owner thread has to be scheduled, start the write and fill the
// pipe, and none of that is guaranteed to happen inside a second when the
// cores are all spoken for. So the precondition is observed instead — a probe
// that fails to acquire the writer is the writer being held, which is exactly
// what the waiter is about to be measured against.
struct WriterDeadlineAttempt {
  bool owner_held_writer{};
  std::string waiter_error;
  std::string owner_error;
  bool server_alive{};
  bool waiter_within_deadline{};
  std::size_t waiter_operations{};
  Attribution waiter_attribution{Attribution::exact};
  bool waiter_block{};
};

// The error a request gets when someone else holds the writer, which is the
// one thing that says the owner is where this test needs it.
constexpr std::string_view kWriterHeld = "control writer acquisition deadline expired";

// Poll until a probe cannot acquire the writer, or give up.
//
// A probe that acquires it instead reaches a stopped client and expires after
// dispatch, which costs its deadline and says the owner is not blocked yet.
// Either way this only ever adds already-expired requests to a connection the
// attempt is about to shut down.
bool wait_until_writer_is_held(Connection& connection) {
  // Detection is sub-second when the owner blocks at all, so this is a wide
  // margin rather than a guess at a scheduling delay. It is also bounded by
  // what the whole binary is allowed: five of these must still fit inside the
  // test's timeout, or a broken build reports as a timeout instead of saying
  // which assertion went.
  const auto give_up = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < give_up) {
    const auto probe =
        connection.execute(group({{"display-message", "-p", "writer-probe"}}),
                           std::chrono::steady_clock::now() + 50ms);
    if (probe.connection_error.has_value() &&
        probe.connection_error->message == kWriterHeld) {
      return true;
    }
  }
  return false;
}

WriterDeadlineAttempt run_writer_deadline_attempt(std::size_t index) {
  WriterDeadlineAttempt attempt;
  auto server =
      start_server(unique_name("control-writer-deadline-" + std::to_string(index)));
  if (!server.has_value()) {
    return attempt;
  }
  auto connected = connect_to(*server);
  if (!connected.has_value()) {
    return attempt;
  }
  auto connection = std::move(*connected);
  const auto child_pid = connection.native_child_pid();
  if (child_pid <= 0 || ::kill(static_cast<pid_t>(child_pid), SIGSTOP) != 0) {
    return attempt;
  }

  ControlRequest large_request;
  large_request.group.push_back(ControlCommand{
      .argv = {"display-message", "-p", std::string(4U * 1024U * 1024U, 'x')}});
  std::optional<ControlRequestResult> owner_result;
  std::barrier started{2};
  std::thread owner_thread{[&] {
    started.arrive_and_wait();
    owner_result = connection.execute(std::move(large_request),
                                      std::chrono::steady_clock::now() + 10s);
  }};
  started.arrive_and_wait();
  if (!wait_until_writer_is_held(connection)) {
    // The owner never blocked, so there is no contention to measure. Reported
    // as such: the caller retries rather than asserting on a scenario that
    // did not happen.
    static_cast<void>(connection.shutdown(std::chrono::steady_clock::now() + 2s));
    owner_thread.join();
    return attempt;
  }

  const auto waiter_deadline = std::chrono::steady_clock::now() + 200ms;
  const auto waiter_result = connection.execute(
      group({{"display-message", "-p", "must-not-dispatch"}}), waiter_deadline);
  const auto waiter_finished = std::chrono::steady_clock::now();
  static_cast<void>(connection.shutdown(std::chrono::steady_clock::now() + 2s));
  owner_thread.join();

  attempt.waiter_within_deadline = waiter_finished <= waiter_deadline + 100ms;
  attempt.waiter_operations = waiter_result.operations.size();
  if (!waiter_result.operations.empty()) {
    attempt.waiter_attribution = waiter_result.operations[0].attribution;
    attempt.waiter_block = waiter_result.operations[0].block.has_value();
  }
  if (waiter_result.connection_error.has_value()) {
    attempt.waiter_error = waiter_result.connection_error->message;
  }
  if (owner_result.has_value() && owner_result->connection_error.has_value()) {
    attempt.owner_error = owner_result->connection_error->message;
  }
  attempt.owner_held_writer = attempt.waiter_error == kWriterHeld;
  attempt.server_alive = server->is_alive();
  return attempt;
}

TEST(ControlModeConnection, WriterWaitHonorsDeadlineWithoutPoisoningOwner) {
  // Fewer than before: the probe makes the owner blocking observable rather
  // than likely, so a retry now only covers the owner never blocking at all.
  constexpr std::size_t kAttempts = 3;
  WriterDeadlineAttempt attempt;
  std::size_t used = 0;
  for (; used < kAttempts; ++used) {
    attempt = run_writer_deadline_attempt(used);
    if (attempt.owner_held_writer) {
      break;
    }
  }
  ASSERT_TRUE(attempt.owner_held_writer)
      << "the owner never held the writer across " << kAttempts
      << " attempts; last waiter error was: " << attempt.waiter_error;

  EXPECT_TRUE(attempt.waiter_within_deadline);
  ASSERT_EQ(attempt.waiter_operations, 1U);
  EXPECT_EQ(attempt.waiter_attribution, Attribution::unknown);
  EXPECT_FALSE(attempt.waiter_block);
  // The owner is ended by shutdown, never by the waiter's timeout.
  EXPECT_EQ(attempt.owner_error, "control connection shut down");
  EXPECT_TRUE(attempt.server_alive);
}

TEST(ControlModeConnection, ExternallyTerminatedClientIsReapedWhileOwned) {
  auto server = start_server(unique_name("control-external-termination"));
  ASSERT_TRUE(server.has_value()) << (server.has_value() ? "" : server.error());
  auto connected = connect_to(*server);
  ASSERT_TRUE(connected.has_value())
      << (connected.has_value() ? "" : connected.error().message);
  auto connection = std::move(*connected);
  const auto child_pid = connection.native_child_pid();
  ASSERT_GT(child_pid, 0);

  ControlRequest request;
  request.group.push_back(ControlCommand{
      .argv = {"display-message", "-p", std::string(4U * 1024U * 1024U, 'x')}});
  std::optional<ControlRequestResult> result;
  std::barrier started{2};
  std::thread request_thread{[&] {
    started.arrive_and_wait();
    result =
        connection.execute(std::move(request), std::chrono::steady_clock::now() + 10s);
  }};
  const auto stopped = ::kill(static_cast<pid_t>(child_pid), SIGSTOP) == 0;
  started.arrive_and_wait();
  std::optional<int> queued;
  const auto dispatch_deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < dispatch_deadline) {
    queued = queued_control_input(static_cast<pid_t>(child_pid));
    if (queued && *queued > 0) {
      break;
    }
    std::this_thread::sleep_for(1ms);
  }
  const auto partial_written = queued && *queued > 0;
  const auto killed = ::kill(-static_cast<pid_t>(child_pid), SIGKILL) == 0;
  request_thread.join();

  ASSERT_TRUE(stopped);
  ASSERT_TRUE(partial_written);
  ASSERT_TRUE(killed);
  ASSERT_TRUE(result.has_value());
  const auto& completed = required_value(result);
  ASSERT_EQ(completed.operations.size(), 1U);
  EXPECT_EQ(completed.operations[0].attribution, Attribution::unknown);
  EXPECT_FALSE(completed.operations[0].block.has_value());
  EXPECT_TRUE(completed.connection_error.has_value());

  bool reaped = false;
  const auto reap_deadline = std::chrono::steady_clock::now() + 2s;
  while (!reaped && std::chrono::steady_clock::now() < reap_deadline) {
    siginfo_t child{};
    errno = 0;
    const auto observed = ::waitid(P_PID, static_cast<id_t>(child_pid), &child,
                                   WEXITED | WNOHANG | WNOWAIT);
    reaped = observed < 0 && errno == ECHILD;
    if (!reaped) {
      std::this_thread::sleep_for(1ms);
    }
  }
  EXPECT_TRUE(reaped);
  const auto later =
      connection.execute(group({{"display-message", "-p", "must-not-dispatch"}}),
                         std::chrono::steady_clock::now() + 2s);
  ASSERT_EQ(later.operations.size(), 1U);
  EXPECT_EQ(later.operations[0].attribution, Attribution::unknown);
  EXPECT_FALSE(later.operations[0].block.has_value());
  EXPECT_TRUE(later.connection_error.has_value());
  static_cast<void>(connection.shutdown(std::chrono::steady_clock::now() + 2s));
  EXPECT_TRUE(server->is_alive());
}

TEST(ControlModeConnection, DeadlineMarksUnresolvedUnknownAndPoisonsLaterAttribution) {
  auto server = start_server(unique_name("control-deadline"));
  ASSERT_TRUE(server.has_value()) << (server.has_value() ? "" : server.error());
  auto connected = connect_to(*server);
  ASSERT_TRUE(connected.has_value())
      << (connected.has_value() ? "" : connected.error().message);
  auto connection = std::move(*connected);

  const auto timed_out = connection.execute(
      group({{"run-shell", "sleep 5"}, {"display-message", "-p", "late-old-request"}}),
      std::chrono::steady_clock::now() + 200ms);
  ASSERT_EQ(timed_out.operations.size(), 2U);
  EXPECT_TRUE(timed_out.operations[0].attribution == Attribution::exact ||
              timed_out.operations[0].attribution == Attribution::unknown);
  EXPECT_EQ(timed_out.operations[0].block.has_value(),
            timed_out.operations[0].attribution == Attribution::exact);
  EXPECT_EQ(timed_out.operations[1].attribution, Attribution::unknown);
  EXPECT_FALSE(timed_out.operations[1].block.has_value());
  EXPECT_TRUE(timed_out.connection_error.has_value());

  const auto later =
      connection.execute(group({{"display-message", "-p", "new-request"}}),
                         std::chrono::steady_clock::now() + 1s);
  ASSERT_EQ(later.operations.size(), 1U);
  EXPECT_EQ(later.operations[0].attribution, Attribution::unknown);
  EXPECT_FALSE(later.operations[0].block.has_value());
  EXPECT_TRUE(later.connection_error.has_value());
  static_cast<void>(connection.shutdown(std::chrono::steady_clock::now() + 2s));
}

// The build records which tmux binary the suite resolved, so a result can say
// what produced it. What is worth pinning is that it recorded *something*
// usable — an absolute path to a file that is there, and a well-formed digest
// of it. Pinning a particular digest would pin the machine the test was
// written on: every other machine, and every tmux in the compatibility matrix,
// has a different binary and would fail a test that has nothing to say about
// them.
TEST(ControlModeConnection, ResolvedTmuxIdentityIsBound) {
  const std::filesystem::path resolved{LIBTMUX_CONTROL_TMUX_PATH};
  EXPECT_TRUE(resolved.is_absolute()) << resolved;
  EXPECT_TRUE(std::filesystem::exists(resolved)) << resolved;

  const std::string_view digest{LIBTMUX_CONTROL_TMUX_SHA256};
  EXPECT_EQ(digest.size(), 64U) << digest;
  EXPECT_TRUE(std::ranges::all_of(digest, [](const char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f');
  })) << digest;
}

} // namespace
