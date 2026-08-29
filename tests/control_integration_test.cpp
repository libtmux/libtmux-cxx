#include "libtmux/control.hpp"
#include "libtmux/expected.hpp"
#include "libtmux/server.hpp"

#include "libtmux/testing/capabilities.hpp"
#include "libtmux/testing/scoped_server.hpp"
#include "support/descriptors.hpp"
#include "support/platform.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include <array>

#include <poll.h>
#include <pthread.h>

#include <gtest/gtest.h>

#ifndef LIBTMUX_CONTROL_TMUX_PATH
#error "LIBTMUX_CONTROL_TMUX_PATH must name the tested tmux executable"
#endif
#ifndef LIBTMUX_CONTROL_TMUX_SHA256
#error "LIBTMUX_CONTROL_TMUX_SHA256 must bind the tested tmux executable"
#endif

namespace {

using namespace std::chrono_literals;
using libtmux::Connection;
using libtmux::ConnectionOptions;
using libtmux::ControlCommand;
using libtmux::ControlRequest;
using libtmux::ControlRequestResult;
using libtmux::ControlTerminal;
using libtmux::DeliveryStatus;
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
  ASSERT_EQ(result.blocks.size(), 1U);
  EXPECT_EQ(result.blocks[0].terminal, ControlTerminal::end);
  EXPECT_EQ(text(result.blocks[0].body), expected_body);
}

bool has_notification(const std::vector<Notification>& notifications,
                      std::string_view prefix) {
  return std::ranges::any_of(notifications, [prefix](const auto& notification) {
    return text(notification.body).starts_with(prefix);
  });
}

#if defined(__linux__)
// No portable call reports another process's signal mask, and /proc gives it
// as a hex word.
std::optional<unsigned long long> blocked_signals(int pid) {
  std::ifstream status{"/proc/" + std::to_string(pid) + "/status"};
  std::string line;
  while (std::getline(status, line)) {
    constexpr std::string_view field{"SigBlk:"};
    if (line.starts_with(field)) {
      return std::stoull(line.substr(field.size()), nullptr, 16);
    }
  }
  return std::nullopt;
}

// tmux restores the mask it inherited rather than an empty one, so a signal
// blocked here stays blocked in the server and in every pane command that does
// not set a mask of its own. A server that cannot be terminated by SIGTERM
// outlives whatever was supposed to stop it.
TEST(ControlModeConnection, ServerDoesNotInheritABlockedSignalMask) {
  sigset_t blocked;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGTERM);
  sigset_t previous;
  ASSERT_EQ(::pthread_sigmask(SIG_BLOCK, &blocked, &previous), 0);
  auto server = start_server(unique_name("control-signal-mask"));
  ASSERT_EQ(::pthread_sigmask(SIG_SETMASK, &previous, nullptr), 0);
  ASSERT_TRUE(server.has_value()) << (server.has_value() ? "" : server.error());

  auto connected = connect_to(*server);
  ASSERT_TRUE(connected.has_value())
      << (connected.has_value() ? "" : connected.error().message);
  auto connection = std::move(*connected);

  const auto reported = connection.execute(group({{"display-message", "-p", "#{pid}"}}),
                                           std::chrono::steady_clock::now() + 2s);
  ASSERT_FALSE(reported.connection_error.has_value());
  ASSERT_EQ(reported.blocks.size(), 1U);
  const auto server_pid = std::stoi(text(reported.blocks[0].body));

  const auto mask = blocked_signals(server_pid);
  ASSERT_TRUE(mask.has_value());
  EXPECT_EQ(*mask, 0ULL);
}
#endif

// The other spawn: the server starts outside the block, so only the control
// client this library launches is under it.
TEST(ControlModeConnection, ControlClientDoesNotInheritABlockedSignalMask) {
  auto server = start_server(unique_name("control-client-mask"));
  ASSERT_TRUE(server.has_value()) << (server.has_value() ? "" : server.error());

  sigset_t blocked;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGTERM);
  sigset_t previous;
  ASSERT_EQ(::pthread_sigmask(SIG_BLOCK, &blocked, &previous), 0);
  auto connected = connect_to(*server);
  ASSERT_EQ(::pthread_sigmask(SIG_SETMASK, &previous, nullptr), 0);
  ASSERT_TRUE(connected.has_value())
      << (connected.has_value() ? "" : connected.error().message);
  auto connection = std::move(*connected);

  const auto reported =
      connection.execute(group({{"display-message", "-p", "#{client_pid}"}}),
                         std::chrono::steady_clock::now() + 2s);
  ASSERT_FALSE(reported.connection_error.has_value());
  ASSERT_EQ(reported.blocks.size(), 1U);
  const auto client_pid = std::stoi(text(reported.blocks[0].body));

  const auto mask = blocked_signals(client_pid);
  ASSERT_TRUE(mask.has_value());
  EXPECT_EQ(*mask, 0ULL);
}

TEST(ControlModeConnection, RejectsBoundsTooSmallForItsPrivateBoundary) {
  auto server = start_server(unique_name("control-boundary-bounds"));
  ASSERT_TRUE(server.has_value()) << (server.has_value() ? "" : server.error());
  ConnectionOptions options{.tmux_binary = LIBTMUX_CONTROL_TMUX_PATH,
                            .socket_path = server->socket_path(),
                            .session_name = std::string{server->session_name()},
                            .startup_timeout = 2s,
                            .shutdown_timeout = 2s};

  options.retained_reply_bytes = 127U;
  auto short_reply = Connection::connect(options);
  ASSERT_FALSE(short_reply.has_value());
  EXPECT_NE(short_reply.error().message.find("at least 128 bytes"), std::string::npos);

  options.retained_reply_bytes = libtmux::kDefaultRetainedReplyBytes;
  options.line_bytes = 127U;
  auto short_line = Connection::connect(std::move(options));
  ASSERT_FALSE(short_line.has_value());
  EXPECT_NE(short_line.error().message.find("at least 128 bytes"), std::string::npos);
}

TEST(ControlModeConnection, FailFastGroupStopsAtTheErrorBoundary) {
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
  ASSERT_EQ(result.blocks.size(), 1U);
  EXPECT_EQ(result.blocks[0].terminal, ControlTerminal::error);
  EXPECT_NE(text(result.blocks[0].body).find("can't find session"), std::string::npos);

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
    auto available = connection.wait_for_notifications(notification_deadline);
    if (available.empty()) {
      break;
    }
    notifications.insert(notifications.end(),
                         std::make_move_iterator(available.begin()),
                         std::make_move_iterator(available.end()));
  }
  EXPECT_TRUE(has_notification(notifications, "%window-add "));
  EXPECT_TRUE(connection.shutdown(std::chrono::steady_clock::now() + 2s).has_value());
}

TEST(ControlModeConnection, AliasExpansionKeepsEveryReplyAndTheNextRequestAligned) {
  auto fixture = start_server(unique_name("control-alias-boundary"));
  ASSERT_TRUE(fixture.has_value()) << (fixture.has_value() ? "" : fixture.error());
  auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  const auto configured = server->run(
      {"set-option", "-s", "command-alias[100]",
       "nested-reply=display-message -p alias-one ; display-message -p alias-two"});
  ASSERT_TRUE(configured.has_value()) << configured.error().diagnostic;

  auto connected = connect_to(*fixture);
  ASSERT_TRUE(connected.has_value())
      << (connected.has_value() ? "" : connected.error().message);
  auto connection = std::move(*connected);

  const auto expanded = connection.execute(group({{"nested-reply"}}),
                                           std::chrono::steady_clock::now() + 2s);
  ASSERT_FALSE(expanded.connection_error.has_value())
      << (expanded.connection_error ? expanded.connection_error->message : "");
  ASSERT_EQ(expanded.blocks.size(), 2U);
  for (const auto& block : expanded.blocks) {
    EXPECT_EQ(block.terminal, ControlTerminal::end);
  }
  EXPECT_EQ(text(expanded.blocks[0].body), "alias-one\n");
  EXPECT_EQ(text(expanded.blocks[1].body), "alias-two\n");

  const auto after =
      connection.execute(group({{"display-message", "-p", "still-aligned"}}),
                         std::chrono::steady_clock::now() + 2s);
  expect_exact_end(after, "still-aligned\n");
  EXPECT_TRUE(connection.shutdown(std::chrono::steady_clock::now() + 2s).has_value());
}

TEST(ControlModeConnection, InsertedReplyStaysWithItsConcurrentRequest) {
  auto server = start_server(unique_name("control-explicit-replies"));
  ASSERT_TRUE(server.has_value()) << (server.has_value() ? "" : server.error());
  auto connected = connect_to(*server);
  ASSERT_TRUE(connected.has_value())
      << (connected.has_value() ? "" : connected.error().message);
  auto connection = std::move(*connected);

  std::optional<ControlRequestResult> inserted;
  std::optional<ControlRequestResult> marker;
  std::barrier ready{3};
  std::thread inserted_thread{[&] {
    ready.arrive_and_wait();
    inserted = connection.execute(
        group({{"if-shell", "-F", "1", "display-message -p inserted-marker"}}),
        std::chrono::steady_clock::now() + 2s);
  }};
  std::thread marker_thread{[&] {
    ready.arrive_and_wait();
    marker = connection.execute(group({{"display-message", "-p", "next-marker"}}),
                                std::chrono::steady_clock::now() + 2s);
  }};
  ready.arrive_and_wait();
  inserted_thread.join();
  marker_thread.join();

  ASSERT_TRUE(inserted.has_value());
  ASSERT_FALSE(inserted->connection_error.has_value())
      << (inserted->connection_error ? inserted->connection_error->message : "");
  ASSERT_EQ(inserted->blocks.size(), 2U);
  for (const auto& block : inserted->blocks) {
    EXPECT_EQ(block.terminal, ControlTerminal::end);
  }
  EXPECT_TRUE(inserted->blocks[0].body.empty());
  EXPECT_EQ(text(inserted->blocks[1].body), "inserted-marker\n");

  ASSERT_TRUE(marker.has_value());
  expect_exact_end(*marker, "next-marker\n");
  const auto after =
      connection.execute(group({{"display-message", "-p", "after-marker"}}),
                         std::chrono::steady_clock::now() + 2s);
  expect_exact_end(after, "after-marker\n");
  EXPECT_TRUE(connection.shutdown(std::chrono::steady_clock::now() + 2s).has_value());
}

// Waiting, rather than asking repeatedly and sleeping in between.
TEST(ControlModeConnection, WaitForNotificationsWakesOnTheEventNotTheDeadline) {
  auto server = start_server(unique_name("control-wait"));
  ASSERT_TRUE(server.has_value()) << (server.has_value() ? "" : server.error());
  auto connected = connect_to(*server);
  ASSERT_TRUE(connected.has_value())
      << (connected.has_value() ? "" : connected.error().message);
  auto connection = std::move(*connected);

  // Drain whatever attaching produced, so the wait below is for the new event.
  static_cast<void>(
      connection.wait_for_notifications(std::chrono::steady_clock::now() + 250ms));
  static_cast<void>(connection.take_notifications());

  const auto created =
      connection.execute(group({{"new-window", "-d", "-n", "waited-for"}}),
                         std::chrono::steady_clock::now() + 2s);
  ASSERT_FALSE(created.connection_error.has_value());

  // A deadline far past when the event should land: returning long before it
  // is what shows the wait was woken rather than timed out.
  const auto generous = std::chrono::steady_clock::now() + 10s;
  const auto started = std::chrono::steady_clock::now();
  std::vector<Notification> notifications;
  while (std::chrono::steady_clock::now() < generous &&
         !has_notification(notifications, "%window-add ")) {
    auto available = connection.wait_for_notifications(generous);
    if (available.empty()) {
      break;
    }
    notifications.insert(notifications.end(),
                         std::make_move_iterator(available.begin()),
                         std::make_move_iterator(available.end()));
  }
  const auto waited = std::chrono::steady_clock::now() - started;

  EXPECT_TRUE(has_notification(notifications, "%window-add "));
  EXPECT_LT(waited, 5s) << "woke on the deadline rather than the event";
  EXPECT_TRUE(connection.shutdown(std::chrono::steady_clock::now() + 2s).has_value());
}

TEST(ControlModeConnection, NotificationWatchesDoNotStealFromEachOther) {
  auto server = start_server(unique_name("control-watch"));
  ASSERT_TRUE(server.has_value()) << (server.has_value() ? "" : server.error());
  auto connected = connect_to(*server);
  ASSERT_TRUE(connected.has_value())
      << (connected.has_value() ? "" : connected.error().message);
  auto connection = std::move(*connected);

  static_cast<void>(connection.take_notifications());
  auto first = connection.watch_notifications();
  auto second = connection.watch_notifications();

  const auto created =
      connection.execute(group({{"new-window", "-d", "-n", "watched"}}),
                         std::chrono::steady_clock::now() + 2s);
  ASSERT_FALSE(created.connection_error.has_value());

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  const auto first_events = first.wait_for_notifications(deadline);
  std::array<pollfd, 1> second_ready{
      pollfd{.fd = second.notification_fd(), .events = POLLIN, .revents = 0}};
  ASSERT_GE(second_ready.front().fd, 0);
  ASSERT_EQ(::poll(second_ready.data(), second_ready.size(), 0), 1);
  EXPECT_NE(second_ready.front().revents & POLLIN, 0);
  const auto second_events = second.wait_for_notifications(deadline);
  const auto legacy_events = connection.wait_for_notifications(deadline);

  EXPECT_TRUE(has_notification(first_events, "%window-add "));
  EXPECT_TRUE(has_notification(second_events, "%window-add "));
  EXPECT_TRUE(has_notification(legacy_events, "%window-add "));
  EXPECT_EQ(first.dropped_notifications(), 0U);
  EXPECT_EQ(second.dropped_notifications(), 0U);
  EXPECT_TRUE(connection.shutdown(std::chrono::steady_clock::now() + 2s).has_value());
}

TEST(ControlModeConnection, NotificationWatchDrainsAfterConnectionDestruction) {
  auto server = start_server(unique_name("control-watch-lifetime"));
  ASSERT_TRUE(server.has_value()) << (server.has_value() ? "" : server.error());
  libtmux::NotificationWatch watch;
  {
    auto connected = connect_to(*server);
    ASSERT_TRUE(connected.has_value())
        << (connected.has_value() ? "" : connected.error().message);
    auto connection = std::move(*connected);
    watch = connection.watch_notifications();

    const auto created =
        connection.execute(group({{"new-window", "-d", "-n", "retained"}}),
                           std::chrono::steady_clock::now() + 2s);
    ASSERT_FALSE(created.connection_error.has_value());
    EXPECT_TRUE(connection.shutdown(std::chrono::steady_clock::now() + 2s).has_value());
  }

  const auto retained = watch.take_notifications();
  EXPECT_TRUE(has_notification(retained, "%window-add "));
  std::array<pollfd, 1> closed{
      pollfd{.fd = watch.notification_fd(), .events = POLLIN, .revents = 0}};
  ASSERT_GE(closed.front().fd, 0);
  ASSERT_EQ(::poll(closed.data(), closed.size(), 0), 1);
  EXPECT_NE(closed.front().revents & POLLIN, 0);
}

// The other half: an empty answer must mean the deadline was reached.
//
// Stated as that implication rather than as "tmux says nothing for 300ms",
// which is a claim about tmux rather than about this call — and a false one on
// tmux master, which is still talking after the others have gone quiet. Every
// non-empty answer here is a legitimate early wake and is left to the test
// above; this one retries until it sees the deadline path, which is the case
// it exists for.
TEST(ControlModeConnection, WaitForNotificationsReturnsEmptyOnlyAtItsDeadline) {
  auto server = start_server(unique_name("control-wait"));
  ASSERT_TRUE(server.has_value()) << (server.has_value() ? "" : server.error());
  auto connected = connect_to(*server);
  ASSERT_TRUE(connected.has_value())
      << (connected.has_value() ? "" : connected.error().message);
  auto connection = std::move(*connected);

  constexpr auto window = 300ms;
  bool saw_deadline = false;
  const auto give_up = std::chrono::steady_clock::now() + 10s;
  while (std::chrono::steady_clock::now() < give_up && !saw_deadline) {
    const auto started = std::chrono::steady_clock::now();
    const auto batch =
        connection.wait_for_notifications(std::chrono::steady_clock::now() + window);
    const auto waited = std::chrono::steady_clock::now() - started;
    if (batch.empty()) {
      // Generously under the window, so a loaded runner cannot fail this while
      // a version that returned immediately still would.
      EXPECT_GE(waited, window - 50ms) << "returned empty without waiting";
      saw_deadline = true;
    }
  }
  EXPECT_TRUE(saw_deadline) << "tmux never paused long enough to reach a deadline";
  EXPECT_TRUE(connection.shutdown(std::chrono::steady_clock::now() + 2s).has_value());
}

// Pane output arrives only when the connection asked for it at connect time.
//
// Both halves matter and neither is obvious. A connection that did not ask
// cannot be made to listen — tmux ignores `refresh-client -A "%N:on"` on a
// client started with `no-output`, which is why this is an option rather than
// a subscription. The measurements behind that are in
// `docs/design/pane-output-streaming.md`.
TEST(ControlModeConnection, ServerOwnsTheRouteAndPreservesStreamPolicy) {
  auto fixture = start_server(unique_name("control-server-options"));
  ASSERT_TRUE(fixture.has_value()) << (fixture.has_value() ? "" : fixture.error());

  auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;

  auto connected = server->control_with_options(
      fixture->session_name(), {.tmux_binary = LIBTMUX_CONTROL_TMUX_PATH,
                                .socket_path = "/caller-route-must-be-ignored",
                                .session_name = "caller-session-must-be-ignored",
                                .startup_timeout = 2s,
                                .shutdown_timeout = 2s,
                                .pane_output = true,
                                .pause_after = 17s});
  ASSERT_TRUE(connected.has_value())
      << (connected.has_value() ? "" : connected.error().message);
  auto connection = std::move(*connected);

  const auto typed = connection.execute(
      group({{"send-keys", "-t", std::string{fixture->session_name()},
              "echo server-routed-stream", "Enter"}}),
      std::chrono::steady_clock::now() + 2s);
  ASSERT_FALSE(typed.connection_error.has_value())
      << (typed.connection_error ? typed.connection_error->message : "");

  bool saw_output = false;
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline && !saw_output) {
    for (const Notification& notification :
         connection.wait_for_notifications(deadline)) {
      // `pause-after` makes tmux report pane bytes as `%extended-output`.
      saw_output = saw_output || libtmux::parse(notification).kind ==
                                     libtmux::NotificationKind::extended_output;
    }
  }
  EXPECT_TRUE(saw_output);
  EXPECT_TRUE(connection.shutdown(std::chrono::steady_clock::now() + 2s).has_value());
}

TEST(ControlModeConnection, DeliversPaneOutputOnlyWhenAskedAtConnectTime) {
  auto server = start_server(unique_name("control-output"));
  ASSERT_TRUE(server.has_value()) << (server.has_value() ? "" : server.error());

  const auto collect = [&](bool pane_output) {
    auto connected =
        Connection::connect({.tmux_binary = LIBTMUX_CONTROL_TMUX_PATH,
                             .socket_path = server->socket_path(),
                             .session_name = std::string{server->session_name()},
                             .startup_timeout = 2s,
                             .shutdown_timeout = 2s,
                             .pane_output = pane_output});
    EXPECT_TRUE(connected.has_value())
        << (connected.has_value() ? "" : connected.error().message);
    if (!connected.has_value()) {
      return 0;
    }
    auto connection = std::move(*connected);

    const auto typed = connection.execute(
        group({{"send-keys", "-t", std::string{server->session_name()}, "echo hi",
                "Enter"}}),
        std::chrono::steady_clock::now() + 2s);
    static_cast<void>(typed);

    int outputs = 0;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto batch = connection.wait_for_notifications(deadline);
      if (batch.empty()) {
        break;
      }
      for (const Notification& notification : batch) {
        if (text(notification.body).starts_with("%output ")) {
          ++outputs;
        }
      }
      if (outputs > 0) {
        break;
      }
    }
    static_cast<void>(connection.shutdown(std::chrono::steady_clock::now() + 2s));
    return outputs;
  };

  EXPECT_EQ(collect(false), 0) << "output arrived on a connection that never asked";
  EXPECT_GT(collect(true), 0) << "asked for output and none arrived";
}

// The parser against what tmux actually sends, rather than what its source
// suggests it sends.
TEST(ControlModeConnection, ParsesTheNotificationsARealServerEmits) {
  auto server = start_server(unique_name("control-parsed"));
  ASSERT_TRUE(server.has_value()) << (server.has_value() ? "" : server.error());
  auto connected = connect_to(*server);
  ASSERT_TRUE(connected.has_value())
      << (connected.has_value() ? "" : connected.error().message);
  auto connection = std::move(*connected);

  const auto made =
      connection.execute(group({{"new-window", "-d", "-n", "parsed-window"}}),
                         std::chrono::steady_clock::now() + 2s);
  ASSERT_FALSE(made.connection_error.has_value());

  bool saw_window_add = false;
  std::string added_window;
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline && !saw_window_add) {
    const auto batch = connection.wait_for_notifications(deadline);
    if (batch.empty()) {
      break;
    }
    for (const Notification& notification : batch) {
      const auto parsed = libtmux::parse(notification);
      // Nothing a real server sends may parse as a name this build cannot
      // read; the set only grows, and this tmux is inside the supported range.
      EXPECT_NE(parsed.name.size(), 0U);
      EXPECT_EQ(parsed.name.front(), '%');
      if (parsed.kind == libtmux::NotificationKind::window_add) {
        saw_window_add = true;
        added_window = std::string{parsed.window};
      }
    }
  }

  EXPECT_TRUE(saw_window_add);
  ASSERT_FALSE(added_window.empty());
  EXPECT_EQ(added_window.front(), '@') << added_window;
  EXPECT_TRUE(connection.shutdown(std::chrono::steady_clock::now() + 2s).has_value());
}

// The descriptor, used the way it exists to be used: one `poll` over tmux and
// something else, on one thread, with nothing blocked on the connection.
TEST(ControlModeConnection, NotificationFdPollsBesideAnotherDescriptor) {
  auto server = start_server(unique_name("control-poll"));
  ASSERT_TRUE(server.has_value()) << (server.has_value() ? "" : server.error());
  auto connected = connect_to(*server);
  ASSERT_TRUE(connected.has_value())
      << (connected.has_value() ? "" : connected.error().message);
  auto connection = std::move(*connected);

  const int tmux_fd = connection.notification_fd();
  ASSERT_GE(tmux_fd, 0);

  // A second descriptor, standing in for whatever else a caller's loop owns.
  std::array<int, 2> other{-1, -1};
  ASSERT_EQ(::pipe(other.data()), 0);

  static_cast<void>(connection.take_notifications());

  const auto made = connection.execute(group({{"new-window", "-d", "-n", "polled"}}),
                                       std::chrono::steady_clock::now() + 2s);
  ASSERT_FALSE(made.connection_error.has_value());

  bool saw_window_add = false;
  bool saw_other = false;
  const char byte = 1;
  ASSERT_EQ(::write(other[1], &byte, 1), 1);

  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline &&
         !(saw_window_add && saw_other)) {
    std::array<pollfd, 2> watched{
        pollfd{.fd = tmux_fd, .events = POLLIN, .revents = 0},
        pollfd{.fd = other[0], .events = POLLIN, .revents = 0}};
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (::poll(watched.data(), watched.size(), static_cast<int>(remaining.count())) <=
        0) {
      break;
    }
    if ((watched[0].revents & POLLIN) != 0) {
      // Readable means a take will return something, and the take clears it.
      const auto batch = connection.take_notifications();
      EXPECT_FALSE(batch.empty()) << "readable but nothing to take";
      for (const Notification& notification : batch) {
        if (libtmux::parse(notification).kind ==
            libtmux::NotificationKind::window_add) {
          saw_window_add = true;
        }
      }
    }
    if ((watched[1].revents & POLLIN) != 0) {
      char drained = 0;
      static_cast<void>(::read(other[0], &drained, 1));
      saw_other = true;
    }
  }

  EXPECT_TRUE(saw_window_add);
  EXPECT_TRUE(saw_other) << "the caller's own descriptor was starved";

  // Drained, so it must have gone quiet again.
  std::array<pollfd, 1> settled{pollfd{.fd = tmux_fd, .events = POLLIN, .revents = 0}};
  static_cast<void>(::poll(settled.data(), settled.size(), 0));
  EXPECT_EQ(settled[0].revents & POLLIN, 0)
      << "still readable with nothing left to take";

  ::close(other[0]);
  ::close(other[1]);
  EXPECT_TRUE(connection.shutdown(std::chrono::steady_clock::now() + 2s).has_value());
}

// Muting one pane, and the asymmetry that makes muting the only per-pane
// control there is.
TEST(ControlModeConnection, MutesOnePaneAndRefusesToWidenASilentConnection) {
  auto server = start_server(unique_name("control-mute"));
  ASSERT_TRUE(server.has_value()) << (server.has_value() ? "" : server.error());

  // A connection that never asked cannot be widened, whatever it asks for.
  {
    auto silent = connect_to(*server);
    ASSERT_TRUE(silent.has_value())
        << (silent.has_value() ? "" : silent.error().message);
    auto connection = std::move(*silent);
    const auto refused =
        connection.set_pane_output("%0", true, std::chrono::steady_clock::now() + 2s);
    ASSERT_FALSE(refused.has_value());
    EXPECT_NE(refused.error().message.find("did not ask for pane output"),
              std::string::npos)
        << refused.error().message;
    static_cast<void>(connection.shutdown(std::chrono::steady_clock::now() + 2s));
  }

  auto connected =
      Connection::connect({.tmux_binary = LIBTMUX_CONTROL_TMUX_PATH,
                           .socket_path = server->socket_path(),
                           .session_name = std::string{server->session_name()},
                           .startup_timeout = 2s,
                           .shutdown_timeout = 2s,
                           .pane_output = true});
  ASSERT_TRUE(connected.has_value())
      << (connected.has_value() ? "" : connected.error().message);
  auto connection = std::move(*connected);

  const auto listed = connection.execute(group({{"list-panes", "-F", "#{pane_id}"}}),
                                         std::chrono::steady_clock::now() + 2s);
  ASSERT_FALSE(listed.connection_error.has_value());
  ASSERT_FALSE(listed.blocks.empty());
  auto pane = text(listed.blocks.front().body);
  while (!pane.empty() && (pane.back() == '\n' || pane.back() == '\r')) {
    pane.pop_back();
  }
  ASSERT_FALSE(pane.empty());

  const auto muted =
      connection.set_pane_output(pane, false, std::chrono::steady_clock::now() + 2s);
  ASSERT_TRUE(muted.has_value()) << muted.error().message;

  static_cast<void>(connection.take_notifications());
  const auto typed = connection.execute(
      group({{"send-keys", "-t", pane, "echo muted-pane-marker", "Enter"}}),
      std::chrono::steady_clock::now() + 2s);
  ASSERT_FALSE(typed.connection_error.has_value());

  int outputs = 0;
  const auto deadline = std::chrono::steady_clock::now() + 1500ms;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto batch = connection.wait_for_notifications(deadline);
    if (batch.empty()) {
      break;
    }
    for (const Notification& notification : batch) {
      if (libtmux::parse(notification).kind == libtmux::NotificationKind::output) {
        ++outputs;
      }
    }
  }
  EXPECT_EQ(outputs, 0) << "a muted pane still delivered output";

  EXPECT_TRUE(connection.shutdown(std::chrono::steady_clock::now() + 2s).has_value());
}

// The same waiting, as one loop.
TEST(ControlModeConnection, EventsRangeYieldsWhatTheHandLoopWould) {
  auto server = start_server(unique_name("control-events"));
  ASSERT_TRUE(server.has_value()) << (server.has_value() ? "" : server.error());
  auto connected = connect_to(*server);
  ASSERT_TRUE(connected.has_value())
      << (connected.has_value() ? "" : connected.error().message);
  auto connection = std::move(*connected);

  static_cast<void>(connection.take_notifications());
  const auto made = connection.execute(group({{"new-window", "-d", "-n", "ranged"}}),
                                       std::chrono::steady_clock::now() + 2s);
  ASSERT_FALSE(made.connection_error.has_value());

  std::string window;
  for (const auto& event : connection.events(std::chrono::steady_clock::now() + 3s)) {
    EXPECT_FALSE(event.name.empty());
    if (event.kind == libtmux::NotificationKind::window_add) {
      window = std::string{event.window};
      break;
    }
  }
  EXPECT_FALSE(window.empty()) << "the range ended without the window";
  EXPECT_EQ(window.front(), '@') << window;

  // Ends on its own when tmux goes quiet, rather than needing a break.
  std::size_t seen = 0;
  const auto started = std::chrono::steady_clock::now();
  for (const auto& event :
       connection.events(std::chrono::steady_clock::now() + 400ms)) {
    static_cast<void>(event);
    ++seen;
    ASSERT_LT(seen, 10000U) << "the range did not end";
  }
  EXPECT_GE(std::chrono::steady_clock::now() - started, 300ms)
      << "the range ended before its deadline with nothing to report";

  EXPECT_TRUE(connection.shutdown(std::chrono::steady_clock::now() + 2s).has_value());
}

TEST(ControlModeConnection, NotificationShapedCommandOutputRemainsBlockBody) {
  LIBTMUX_REQUIRES_TMUX(3, 4,
                        "keeping notification-shaped command output in the block body");

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

TEST(ControlModeConnection, WaitCapableCommandsFinishAfterTheirGuardedBlocks) {
  auto server = start_server(unique_name("control-delayed-result"));
  ASSERT_TRUE(server.has_value()) << (server.has_value() ? "" : server.error());
  auto connected = connect_to(*server);
  ASSERT_TRUE(connected.has_value())
      << (connected.has_value() ? "" : connected.error().message);
  auto connection = std::move(*connected);
  static_cast<void>(connection.take_notifications());

  const auto shell = connection.execute(group({{"run-shell", "exit 17"}}),
                                        std::chrono::steady_clock::now() + 2s);
  expect_exact_end(shell, "");

  const std::string missing = (server->tmux_tmpdir() / "missing-buffer").string();
  const auto load =
      connection.execute(group({{"load-buffer", "-b", "missing", "--", missing}}),
                         std::chrono::steady_clock::now() + 2s);
  expect_exact_end(load, "");

  // This block cannot arrive until the two waiting commands continue. The
  // reader therefore observed all delayed lines before returning the marker.
  const auto marker =
      connection.execute(group({{"display-message", "-p", "after-delayed-work"}}),
                         std::chrono::steady_clock::now() + 2s);
  expect_exact_end(marker, "after-delayed-work\n");
  const auto outside_blocks = connection.take_notifications();
  const auto contains = [&outside_blocks](std::string_view wanted) {
    return std::ranges::any_of(outside_blocks, [wanted](const Notification& event) {
      return text(event.body).find(wanted) != std::string::npos;
    });
  };

  // The initial `%end` blocks said only that tmux accepted each command. The
  // actual failures are unguarded and have no request identifier, so this raw
  // API exposes wire evidence rather than inventing a final command result.
  EXPECT_TRUE(contains("returned 17"));
  EXPECT_TRUE(contains(missing));
  EXPECT_TRUE(connection.shutdown(std::chrono::steady_clock::now() + 2s).has_value());
}

TEST(ControlModeConnection, EncodesArgumentsWithoutCreatingAnotherCommand) {
  LIBTMUX_SKIP_TMUX_DEFECT(
      3, 4, 3, 5,
      "tmux echoes a non-UTF-8 byte back as its octal escape rather than the byte");

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

TEST(ControlModeConnection, ExecuteVsShutdownCompletesOnceWithOnlyReceivedBlocks) {
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
  EXPECT_LE(completed.blocks.size(), 2U);
  EXPECT_TRUE(completed.connection_error.has_value());
  EXPECT_TRUE(server->is_alive());
}

TEST(ControlModeConnection, LargeSubmitVsShutdownReturnsNoFabricatedBlocks) {
  LIBTMUX_SKIP_WITHOUT_PROCFS("how many bytes are queued on the client's stdin");

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
    queued = libtmux::test::queued_child_stdin_bytes(child_pid);
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
  EXPECT_TRUE(completed.blocks.empty());
  ASSERT_TRUE(completed.connection_error.has_value());
  EXPECT_EQ(completed.connection_error->delivery, DeliveryStatus::indeterminate);
  int status = 0;
  errno = 0;
  EXPECT_EQ(::waitpid(static_cast<pid_t>(child_pid), &status, WNOHANG), -1);
  EXPECT_EQ(errno, ECHILD);
  EXPECT_TRUE(server->is_alive());
}

TEST(ControlModeConnection, PartialWriteShutdownNeverDispatchesTruncatedCommand) {
  LIBTMUX_SKIP_WITHOUT_PROCFS("how many bytes are queued on the client's stdin");

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
    queued = libtmux::test::queued_child_stdin_bytes(child_pid);
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
  EXPECT_TRUE(completed.blocks.empty());
  ASSERT_TRUE(completed.connection_error.has_value());
  EXPECT_EQ(completed.connection_error->delivery, DeliveryStatus::indeterminate);
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

  // The claim is that the second caller honoured its own 200ms deadline rather
  // than waiting for the first caller's two seconds, so the bound only has to
  // separate those two. A hundred milliseconds of slack does not: waking from a
  // timed mutex on a shared runner took 139ms once, which failed a correct
  // shutdown. Half a second is still nowhere near two.
  EXPECT_LE(second_finished, second_deadline + 500ms);
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
  std::size_t waiter_blocks{};
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

  // Same bound, same reason as the concurrent-shutdown test: this separates a
  // waiter that honoured its own 200ms deadline from one that waited for the
  // owner's ten seconds, and the runner's scheduling jitter lives well inside
  // the gap.
  attempt.waiter_within_deadline = waiter_finished <= waiter_deadline + 500ms;
  attempt.waiter_blocks = waiter_result.blocks.size();
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
  // Bounded overall, not just per attempt. Each attempt can spend five seconds
  // deciding the owner never blocked, and three of those plus their own
  // deadlines overran the test's timeout — so a platform where the owner does
  // not block reported as a hang rather than as the assertion below, which
  // says exactly that. A timeout is the one failure that explains nothing.
  const auto stop_retrying = std::chrono::steady_clock::now() + 25s;
  WriterDeadlineAttempt attempt;
  std::size_t used = 0;
  for (; used < kAttempts; ++used) {
    attempt = run_writer_deadline_attempt(used);
    if (attempt.owner_held_writer ||
        std::chrono::steady_clock::now() >= stop_retrying) {
      break;
    }
  }
  ASSERT_TRUE(attempt.owner_held_writer)
      << "the owner never held the writer across " << (used + 1)
      << " attempt(s); last waiter error was: " << attempt.waiter_error;

  EXPECT_TRUE(attempt.waiter_within_deadline);
  EXPECT_EQ(attempt.waiter_blocks, 0U);
  // The owner is ended by shutdown, never by the waiter's timeout.
  EXPECT_EQ(attempt.owner_error, "control connection shut down");
  EXPECT_TRUE(attempt.server_alive);
}

TEST(ControlModeConnection, ExternallyTerminatedClientIsReapedWhileOwned) {
  LIBTMUX_SKIP_WITHOUT_PROCFS("how many bytes are queued on the client's stdin");

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
    queued = libtmux::test::queued_child_stdin_bytes(child_pid);
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
  EXPECT_TRUE(completed.blocks.empty());
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
  EXPECT_TRUE(later.blocks.empty());
  EXPECT_TRUE(later.connection_error.has_value());
  static_cast<void>(connection.shutdown(std::chrono::steady_clock::now() + 2s));
  EXPECT_TRUE(server->is_alive());
}

TEST(ControlModeConnection, DeadlineReturnsOnlyReceivedBlocksAndPoisonsTheConnection) {
  auto server = start_server(unique_name("control-deadline"));
  ASSERT_TRUE(server.has_value()) << (server.has_value() ? "" : server.error());
  auto connected = connect_to(*server);
  ASSERT_TRUE(connected.has_value())
      << (connected.has_value() ? "" : connected.error().message);
  auto connection = std::move(*connected);

  const auto timed_out = connection.execute(
      group({{"run-shell", "sleep 5"}, {"display-message", "-p", "late-old-request"}}),
      std::chrono::steady_clock::now() + 200ms);
  EXPECT_LE(timed_out.blocks.size(), 1U);
  ASSERT_TRUE(timed_out.connection_error.has_value());
  EXPECT_EQ(timed_out.connection_error->delivery, timed_out.blocks.empty()
                                                      ? DeliveryStatus::written
                                                      : DeliveryStatus::replied);

  const auto later =
      connection.execute(group({{"display-message", "-p", "new-request"}}),
                         std::chrono::steady_clock::now() + 1s);
  EXPECT_TRUE(later.blocks.empty());
  ASSERT_TRUE(later.connection_error.has_value());
  EXPECT_EQ(later.connection_error->delivery, DeliveryStatus::not_started);
  static_cast<void>(connection.shutdown(std::chrono::steady_clock::now() + 2s));
}

TEST(ControlModeConnection, ExpiredDeadlineDoesNotStartTheRequest) {
  auto server = start_server(unique_name("control-expired-deadline"));
  ASSERT_TRUE(server.has_value()) << (server.has_value() ? "" : server.error());
  auto connected = connect_to(*server);
  ASSERT_TRUE(connected.has_value())
      << (connected.has_value() ? "" : connected.error().message);
  auto connection = std::move(*connected);

  const auto expired =
      connection.execute(group({{"display-message", "-p", "must-not-dispatch"}}),
                         std::chrono::steady_clock::now() - 1ms);

  EXPECT_TRUE(expired.blocks.empty());
  ASSERT_TRUE(expired.connection_error.has_value());
  EXPECT_EQ(expired.connection_error->delivery, DeliveryStatus::not_started);
  EXPECT_TRUE(connection.shutdown(std::chrono::steady_clock::now() + 2s).has_value());
  EXPECT_TRUE(server->is_alive());
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
