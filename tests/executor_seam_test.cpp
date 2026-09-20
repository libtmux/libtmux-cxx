// A transport a caller supplies, reached through the public headers alone.
//
// `BackendKind::custom` named this from the first release and nothing could
// reach it. What makes the case concrete is control mode: a held-open
// connection answers a listing without launching anything, and
// `docs/design/control-transport.md` refuses it as a *general* command backend
// for reasons that do not touch listings. Exactly twelve tmux commands can
// return `CMD_RETURN_WAIT`, and no `list-*` is among them, so for that closed
// set a guarded block is a complete result.
//
// This file includes nothing from `src/`. That is the test: a consumer can
// write it.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "libtmux/batch.hpp"
#include "libtmux/control.hpp"
#include "libtmux/entities.hpp"
#include "libtmux/server.hpp"
#include "libtmux/snapshot.hpp"
#include "libtmux/testing/scoped_server.hpp"

namespace {

using libtmux::CommandFailure;
using libtmux::CommandRequest;
using libtmux::FailureKind;
using libtmux::Server;
using libtmux::Version;

// Answers a fixed script, so the seam can be tested without any tmux at all.
class ScriptedExecutor final : public libtmux::CommandExecutor {
public:
  explicit ScriptedExecutor(std::string reply) : reply_{std::move(reply)} {}

  [[nodiscard]] libtmux::expected<std::string, CommandFailure>
  run(const CommandRequest& command, std::optional<std::chrono::milliseconds>,
      std::optional<std::size_t>) const override {
    const std::lock_guard held{mutex_};
    seen_.push_back(command.argv());
    return reply_;
  }

  [[nodiscard]] std::vector<std::vector<std::string>> seen() const {
    const std::lock_guard held{mutex_};
    return seen_;
  }

private:
  std::string reply_;
  mutable std::mutex mutex_;
  mutable std::vector<std::vector<std::string>> seen_;
};

// The real case: every listing goes over one held-open control connection.
class ControlExecutor final : public libtmux::CommandExecutor {
public:
  ControlExecutor(libtmux::Connection connection, const Server& fallback)
      : connection_{std::move(connection)}, fallback_{fallback} {}

  [[nodiscard]] libtmux::expected<std::string, CommandFailure>
  run(const CommandRequest& command, std::optional<std::chrono::milliseconds> timeout,
      std::optional<std::size_t> output_limit) const override {
    if (!listing(command)) {
      return fallback_.run(command, timeout, output_limit);
    }

    libtmux::ControlRequest request;
    request.group.push_back(libtmux::ControlCommand{.argv = command.argv()});

    const std::lock_guard held{mutex_};
    auto result =
        connection_.execute(std::move(request), std::chrono::steady_clock::now() +
                                                    std::chrono::seconds{10});
    if (result.connection_error.has_value() || result.blocks.empty()) {
      // An alias could have replaced the listing with something that defers.
      // Failing closed to the launching path keeps the answer right and costs
      // only the launch this was trying to avoid.
      return fallback_.run(command, timeout, output_limit);
    }
    const libtmux::ControlBlock& block = result.blocks.front();
    if (block.terminal == libtmux::ControlTerminal::error) {
      return libtmux::unexpected(
          CommandFailure{.kind = FailureKind::refused,
                         .delivery = libtmux::DeliveryStatus::replied,
                         .exit_code = 1,
                         .diagnostic = "tmux refused it on the control wire"});
    }
    ++over_the_wire_;
    std::string answer;
    answer.reserve(block.body.size());
    for (const std::byte byte : block.body) {
      answer.push_back(static_cast<char>(byte));
    }
    return answer;
  }

  [[nodiscard]] int over_the_wire() const noexcept { return over_the_wire_; }

private:
  // The closed set a guarded block answers completely.
  [[nodiscard]] static bool listing(const CommandRequest& command) {
    if (command.empty()) {
      return false;
    }
    const std::string& verb = command.arguments().front().value();
    return verb == "list-sessions" || verb == "list-windows" || verb == "list-panes" ||
           verb == "list-clients" || verb == "list-buffers" || verb == "list-commands";
  }

  mutable std::mutex mutex_;
  mutable libtmux::Connection connection_;
  Server fallback_;
  mutable int over_the_wire_{};
};

TEST(ExecutorSeam, AServerRunsWhateverTransportItIsGiven) {
  auto executor = std::make_shared<ScriptedExecutor>("");
  auto server =
      Server::over(executor, {.implementation = libtmux::ServerImplementation::tmux,
                              .socket_path = "/nowhere",
                              .version = libtmux::Version{.major = 3, .minor = 7}});
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;

  EXPECT_EQ(server->socket_path(), "/nowhere");
  EXPECT_EQ(server->capabilities().backend, libtmux::BackendKind::custom);
  EXPECT_TRUE(
      server->capabilities().supports(libtmux::ServerFeature::exact_inspection));

  const auto version = server->tmux_version();
  ASSERT_TRUE(version.has_value()) << version.error().diagnostic;
  EXPECT_EQ(version->major, 3U);

  // An empty listing is an empty listing, not a failure.
  const auto sessions = server->sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  EXPECT_TRUE(sessions->empty());

  ASSERT_FALSE(executor->seen().empty());
  EXPECT_EQ(executor->seen().front().front(), "list-sessions");
}

// A batch reaches the transport, flattened. There is no second method to
// implement, which is the point of the narrow seam — but the separators are
// the transport's to carry, and one that dropped them would turn a fail-fast
// group into a single command whose later words read as arguments.
TEST(ExecutorSeam, ABatchArrivesAsOneRequestCarryingItsSeparators) {
  auto executor = std::make_shared<ScriptedExecutor>("");
  auto server =
      Server::over(executor, {.implementation = libtmux::ServerImplementation::tmux,
                              .version = libtmux::Version{.major = 3, .minor = 7}});
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;

  libtmux::CommandBatch batch;
  ASSERT_TRUE(batch.add(CommandRequest{"rename-window", "-t", "@1", "first"}));
  ASSERT_TRUE(batch.add(CommandRequest{"rename-window", "-t", "@2", "second"}));
  ASSERT_TRUE(server->run_batch(batch).has_value());

  const auto seen = executor->seen();
  ASSERT_EQ(seen.size(), 1U) << "a batch is one call, not one call per command";
  const auto& argv = seen.front();
  EXPECT_EQ(std::ranges::count(argv, std::string{libtmux::kCommandSeparator}), 1);
  ASSERT_GE(argv.size(), 5U);
  EXPECT_EQ(argv.front(), "rename-window");
  EXPECT_EQ(argv.at(4), std::string{libtmux::kCommandSeparator});
}

// An attach needs a route back to this exact server, and a caller-supplied
// transport has not given one. Refusing says so; building an argv from the
// empty selector this backend reports would attach to whatever tmux finds.
TEST(ExecutorSeam, AttachIsRefusedRatherThanGuessedAt) {
  // One session row, in the separated form a listing answers with.
  std::string row;
  for (const std::string_view value : {"$0", "work", "1", "1", "/tmp", "0", "", "0"}) {
    row += value;
    row += libtmux::kFormatSeparator;
  }
  auto executor = std::make_shared<ScriptedExecutor>(row + "\n");
  auto server =
      Server::over(executor, {.implementation = libtmux::ServerImplementation::tmux,
                              .version = libtmux::Version{.major = 3, .minor = 7}});
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;

  const auto sessions = server->sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  ASSERT_EQ(sessions->size(), 1U);

  const auto attach = sessions->front().attach_command();
  ASSERT_FALSE(attach.has_value()) << "an executor cannot promise an attach route";
  EXPECT_EQ(attach.error().kind, FailureKind::unsupported);
  EXPECT_EQ(attach.error().delivery, libtmux::DeliveryStatus::not_started);
}

TEST(ExecutorSeam, NoExecutorIsRefusedRatherThanDereferenced) {
  const auto server = Server::over(nullptr);
  ASSERT_FALSE(server.has_value());
  EXPECT_EQ(server.error().kind, FailureKind::validation);
  EXPECT_EQ(server.error().delivery, libtmux::DeliveryStatus::not_started);
}

TEST(ExecutorSeam, ListingsOverAControlConnectionMatchTheLaunchingPath) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const std::string socket = fixture->socket_path().string();

  auto launching = Server::at_socket_path(socket);
  ASSERT_TRUE(launching.has_value());
  const auto sessions = launching->sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  ASSERT_TRUE(sessions->at(0).new_window("alpha").has_value());
  ASSERT_TRUE(sessions->at(0).new_window("beta").has_value());

  libtmux::ConnectionOptions options;
  options.socket_path = socket;
  options.session_name = std::string{sessions->at(0).name()};
  auto connection = libtmux::Connection::connect(std::move(options));
  ASSERT_TRUE(connection.has_value()) << connection.error().message;

  auto executor = std::make_shared<ControlExecutor>(*std::move(connection), *launching);
  auto over_control =
      Server::over(executor, {.implementation = libtmux::ServerImplementation::tmux,
                              .socket_path = socket,
                              .version = libtmux::Version{.major = 3, .minor = 7}});
  ASSERT_TRUE(over_control.has_value()) << over_control.error().diagnostic;

  const auto expected_windows = launching->windows();
  ASSERT_TRUE(expected_windows.has_value()) << expected_windows.error().diagnostic;
  const auto actual_windows = over_control->windows();
  ASSERT_TRUE(actual_windows.has_value()) << actual_windows.error().diagnostic;

  ASSERT_EQ(actual_windows->size(), expected_windows->size());
  for (std::size_t row = 0; row < actual_windows->size(); ++row) {
    EXPECT_EQ(actual_windows->at(row).id().value(),
              expected_windows->at(row).id().value());
    EXPECT_EQ(actual_windows->at(row).name(), expected_windows->at(row).name());
    EXPECT_EQ(actual_windows->at(row).index(), expected_windows->at(row).index());
  }

  const auto panes = over_control->panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  EXPECT_EQ(panes->size(), 3U);

  // Both listings were answered on the wire rather than by a launch.
  EXPECT_GE(executor->over_the_wire(), 2);

  // Anything outside the closed set still reaches tmux the launching way.
  const auto renamed = actual_windows->front().rename("gamma");
  ASSERT_TRUE(renamed.has_value()) << renamed.error().diagnostic;
  const auto after = over_control->windows();
  ASSERT_TRUE(after.has_value()) << after.error().diagnostic;
  EXPECT_EQ(after->front().name(), "gamma");
}

// A POSIX proxy that records each launch and hands off to tmux, so a test can
// count what went over the wire by what did not launch. Named other than
// `tmux`, so the `exec` finds the real one on `PATH`.
struct CountedTmux {
  std::filesystem::path proxy;
  std::filesystem::path records;

  [[nodiscard]] int launches() const {
    std::ifstream file{records};
    int lines = 0;
    for (std::string line; std::getline(file, line);) {
      ++lines;
    }
    return lines;
  }

  void reset() const { std::ofstream{records, std::ios::trunc}; }
};

CountedTmux counted_tmux(const std::filesystem::path& directory) {
  CountedTmux counted{.proxy = directory / "counted-tmux",
                      .records = directory / "counted-tmux.launches"};
  std::ofstream{counted.proxy} << "#!/bin/sh\nprintf '1\\n' >> '"
                               << counted.records.string()
                               << "' || exit 125\nexec tmux \"$@\"\n";
  std::filesystem::permissions(counted.proxy, std::filesystem::perms::owner_all);
  counted.reset();
  return counted;
}

// What `over_control` is for: the typed surface over held-open clients, and
// the same answers as launching. Counted, because a timing proves nothing
// about which path ran.
TEST(ControlBackedServer, BuildsAWindowWithoutLaunchingTmux) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const CountedTmux tmux = counted_tmux(fixture->socket_path().parent_path());
  auto launching = Server::at_socket_path(fixture->socket_path().string(), {},
                                          {.tmux_binary = tmux.proxy});
  ASSERT_TRUE(launching.has_value()) << launching.error().diagnostic;
  auto controlled = launching->over_control(fixture->session_name());
  ASSERT_TRUE(controlled.has_value()) << controlled.error().message;
  EXPECT_EQ(controlled->capabilities().backend, libtmux::BackendKind::control);

  tmux.reset();
  auto sessions = controlled->sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  ASSERT_FALSE(sessions->empty());
  auto window = sessions->front().new_window({.name = "work"});
  ASSERT_TRUE(window.has_value()) << window.error().diagnostic;
  for (int split = 0; split < 5; ++split) {
    const auto pane = window->split();
    ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;
    EXPECT_TRUE(pane->id().value().starts_with('%'))
        << "the -P answer parsed as a pane id";
    ASSERT_TRUE(window->select_layout("tiled").has_value());
  }
  ASSERT_TRUE(window->rename("built").has_value());
  const auto panes = controlled->panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  EXPECT_EQ(tmux.launches(), 0) << "every command here tmux answers inside its block";

  const auto launched = launching->panes();
  ASSERT_TRUE(launched.has_value()) << launched.error().diagnostic;
  ASSERT_EQ(panes->size(), launched->size());
  EXPECT_EQ(panes->size(), 7U);
  for (std::size_t index = 0; index < panes->size(); ++index) {
    EXPECT_EQ((*panes)[index].id().value(), (*launched)[index].id().value());
  }
  const auto renamed = controlled->window(window->id().value());
  ASSERT_TRUE(renamed.has_value()) << renamed.error().diagnostic;
  EXPECT_EQ(renamed->name(), "built");
}

// A command tmux can finish after its block has ended is launched, so the
// answer is the finished one. Over the wire this would read as done while the
// shell was still running.
TEST(ControlBackedServer, LaunchesWhatTmuxMayLeaveRunning) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const CountedTmux tmux = counted_tmux(fixture->socket_path().parent_path());
  auto launching = Server::at_socket_path(fixture->socket_path().string(), {},
                                          {.tmux_binary = tmux.proxy});
  ASSERT_TRUE(launching.has_value()) << launching.error().diagnostic;
  const auto version = launching->tmux_version();
  ASSERT_TRUE(version.has_value()) << version.error().diagnostic;
  auto controlled = launching->over_control(fixture->session_name());
  ASSERT_TRUE(controlled.has_value()) << controlled.error().message;

  tmux.reset();
  const auto finished = controlled->run({"run-shell", "sleep 0.2; printf finished"});
  ASSERT_TRUE(finished.has_value()) << finished.error().diagnostic;
  // Before tmux 3.5, cmd-run-shell.c's cmd_run_shell_print returned without
  // writing when it found no window pane, which is exactly what happens once
  // a control client already holds the session's active pane -- restored in
  // fb37d52d. Below that floor the launch still runs and completes, but the
  // CLI answers empty rather than the shell's output.
  if (*version >= Version{.major = 3, .minor = 5}) {
    EXPECT_EQ(*finished, "finished\n");
  }
  EXPECT_EQ(tmux.launches(), 1);

  // An abbreviation could be anything once aliases are counted, so it launches.
  tmux.reset();
  EXPECT_TRUE(controlled->run({"lsw", "-t", fixture->session_name()}).has_value());
  EXPECT_EQ(tmux.launches(), 1);
}

// A failure reads as the one a launch gives, so the typed layer classifies it
// the same way whichever path answered.
TEST(ControlBackedServer, RefusesInTheShapeALaunchDoes) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  auto launching = Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(launching.has_value()) << launching.error().diagnostic;
  auto controlled = launching->over_control(fixture->session_name());
  ASSERT_TRUE(controlled.has_value()) << controlled.error().message;

  const CommandRequest missing{"split-window", "-t", "%999"};
  const auto wire = controlled->run(missing);
  const auto launched = launching->run(missing);
  ASSERT_FALSE(wire.has_value());
  ASSERT_FALSE(launched.has_value());
  EXPECT_EQ(wire.error().kind, launched.error().kind);
  EXPECT_EQ(wire.error().delivery, launched.error().delivery);
  EXPECT_EQ(wire.error().exit_code, launched.error().exit_code);
  EXPECT_EQ(wire.error().diagnostic, launched.error().diagnostic);
}

// Each client is attached, so a caller counting clients sees them.
TEST(ControlBackedServer, SpreadsCommandsOverTheClientsItWasAskedFor) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  auto launching = Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(launching.has_value()) << launching.error().diagnostic;
  auto controlled = launching->over_control(fixture->session_name(), 4U);
  ASSERT_TRUE(controlled.has_value()) << controlled.error().message;
  const auto clients = controlled->clients();
  ASSERT_TRUE(clients.has_value()) << clients.error().diagnostic;
  EXPECT_EQ(clients->size(), 4U);
  for (int attempt = 0; attempt < 8; ++attempt) {
    EXPECT_TRUE(controlled->sessions().has_value());
  }
  EXPECT_FALSE(launching->over_control(fixture->session_name(), 0U).has_value());
}

} // namespace
