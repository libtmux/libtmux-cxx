// The transport seam, exercised rather than asserted.
//
// The library talks to tmux through one private interface. This test supplies
// a different implementation of it — one that launches nothing and answers
// from a script — and drives the whole public surface over it. That proves two
// things at once: an async or control-mode executor can be dropped in without
// touching an installed header, and the exact argv every operation sends,
// which a test against a live server can only observe indirectly.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "libtmux/entities.hpp"
#include "libtmux/server.hpp"

#include "acquire.hpp"
#include "backend.hpp"
#include "control_backend.hpp"
#include "notification_buffer.hpp"

namespace {

using libtmux::CommandFailure;
using libtmux::expected;
using libtmux::FailureKind;
using libtmux::Server;
using libtmux::Session;
using libtmux::unexpected;

// Answers from a script and records what it was asked, in order.
class ScriptedBackend final : public libtmux::detail::Backend {
public:
  explicit ScriptedBackend(std::vector<std::string> replies)
      : replies_{std::move(replies)} {}

  expected<std::string, CommandFailure>
  run(const std::vector<std::string>& command,
      std::optional<std::chrono::milliseconds> /*timeout*/,
      std::optional<std::size_t> /*output_limit*/) const override {
    issued.push_back(command);
    if (replies_.empty()) {
      return unexpected(CommandFailure{.kind = FailureKind::refused,
                                       .dispatched = true,
                                       .exit_code = 1,
                                       .diagnostic = "the script ran out"});
    }
    std::string reply = replies_.front();
    replies_.erase(replies_.begin());
    return reply;
  }

  const std::vector<std::string>& connection() const noexcept override {
    return connection_;
  }

  expected<libtmux::Version, CommandFailure> version() const override {
    return libtmux::Version{.major = 3, .minor = 7, .revision = 2};
  }

  mutable std::vector<std::vector<std::string>> issued;

private:
  mutable std::vector<std::string> replies_;
  std::vector<std::string> connection_{"-S", "/scripted"};
};

// One row of session fields, in the order the entity asks for them.
std::string session_row(std::string_view id, std::string_view name) {
  const std::string separator{libtmux::kFormatSeparator};
  return std::string{id} + separator + std::string{name} + separator + "1" + separator +
         "2" + separator + "/tmp" + separator + "1700000000" + separator + separator +
         "0" + separator + "\n";
}

TEST(BackendSeam, TheWholeSurfaceRunsOverASubstitutedExecutor) {
  auto backend = std::make_shared<ScriptedBackend>(
      std::vector<std::string>{session_row("$3", "scripted")});
  const Server server = libtmux::detail::server_over(backend);

  const auto sessions = server.sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  ASSERT_EQ(sessions->size(), 1U);

  // Parsed from the script, with no tmux anywhere.
  const Session& session = sessions->front();
  EXPECT_EQ(session.id(), "$3");
  EXPECT_EQ(session.name(), "scripted");
  EXPECT_EQ(session.window_count(), 2);
  EXPECT_EQ(session.path(), "/tmp");

  ASSERT_EQ(backend->issued.size(), 1U);
  EXPECT_EQ(backend->issued.front().at(0), "list-sessions");
  EXPECT_EQ(backend->issued.front().at(1), "-F");
}

TEST(BackendSeam, EveryOperationSendsTheArgvItClaimsTo) {
  auto backend = std::make_shared<ScriptedBackend>(
      std::vector<std::string>{session_row("$0", "work"), "", "", ""});
  const Server server = libtmux::detail::server_over(backend);

  const auto sessions = server.sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  const Session& session = sessions->front();

  ASSERT_TRUE(session.rename("renamed").has_value());
  ASSERT_TRUE(session.set_option("status-position", "top").has_value());
  ASSERT_TRUE(session.kill().has_value());

  ASSERT_EQ(backend->issued.size(), 4U);
  // `--` because a name is data: tmux reads a leading dash as a flag.
  EXPECT_EQ(backend->issued[1],
            (std::vector<std::string>{"rename-session", "-t", "$0", "--", "renamed"}));
  EXPECT_EQ(backend->issued[2], (std::vector<std::string>{"set-option", "-t", "$0",
                                                          "status-position", "top"}));
  EXPECT_EQ(backend->issued[3], (std::vector<std::string>{"kill-session", "-t", "$0"}));
}

TEST(BackendSeam, AnEntityTargetsItsIdRatherThanItsName) {
  // A name that would re-parse as a different target if it were ever used as
  // one. Nothing here may put it in a -t argument.
  auto backend = std::make_shared<ScriptedBackend>(
      std::vector<std::string>{session_row("$7", "left:right.middle"), ""});
  const Server server = libtmux::detail::server_over(backend);

  const auto sessions = server.sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  ASSERT_TRUE(sessions->front().kill().has_value());

  const std::vector<std::string>& killed = backend->issued.back();
  ASSERT_EQ(killed.size(), 3U);
  EXPECT_EQ(killed[2], "$7");
}

TEST(BackendSeam, AFailingExecutorIsReportedNotSwallowed) {
  auto backend = std::make_shared<ScriptedBackend>(std::vector<std::string>{});
  const Server server = libtmux::detail::server_over(backend);

  const auto sessions = server.sessions();
  ASSERT_FALSE(sessions.has_value());
  EXPECT_EQ(sessions.error().kind, FailureKind::refused);
  EXPECT_EQ(sessions.error().diagnostic, "the script ran out");
}

TEST(BackendSeam, AnEmptySuccessfulListingIsNotAlive) {
  auto backend = std::make_shared<ScriptedBackend>(std::vector<std::string>{""});
  const Server server = libtmux::detail::server_over(backend);

  const auto alive = server.check_alive();
  ASSERT_FALSE(alive.has_value());
  EXPECT_EQ(alive.error().kind, FailureKind::refused);
  EXPECT_TRUE(alive.error().dispatched);
  EXPECT_EQ(backend->issued.front(),
            (std::vector<std::string>{"list-sessions", "-F", "#{session_id}"}));
}

TEST(BackendSeam, AnUnknownCustomBackendFailsCapabilitiesClosed) {
  auto backend = std::make_shared<ScriptedBackend>(std::vector<std::string>{});
  const Server server = libtmux::detail::server_over(std::move(backend));

  const auto capabilities = server.capabilities();
  EXPECT_EQ(capabilities.implementation, libtmux::ServerImplementation::unknown);
  EXPECT_EQ(capabilities.backend, libtmux::BackendKind::custom);
  for (const auto feature : {
           libtmux::ServerFeature::exact_inspection,
           libtmux::ServerFeature::server_cleanup,
           libtmux::ServerFeature::control_mode,
           libtmux::ServerFeature::receives_asynchronous_notifications,
       }) {
    EXPECT_FALSE(capabilities.supports(feature));
  }
}

TEST(BackendSeam, SubprocessCapabilitiesAreLocal) {
  std::size_t observed_commands = 0U;
  const auto opened = Server::at_socket_name(
      "libtmux-capabilities-only",
      [&observed_commands](std::string_view, const CommandFailure*) {
        ++observed_commands;
      });
  ASSERT_TRUE(opened.has_value());

  const auto capabilities = opened->capabilities();
  EXPECT_EQ(capabilities.implementation, libtmux::ServerImplementation::tmux);
  EXPECT_EQ(capabilities.backend, libtmux::BackendKind::subprocess);
  EXPECT_TRUE(capabilities.supports(libtmux::ServerFeature::exact_inspection));
  EXPECT_TRUE(capabilities.supports(libtmux::ServerFeature::control_mode));
  EXPECT_FALSE(capabilities.supports(
      libtmux::ServerFeature::receives_asynchronous_notifications));
  EXPECT_EQ(observed_commands, 0U);
}

TEST(BackendSeam, ServerRoutingPreservesControlPolicy) {
  libtmux::ConnectionOptions requested{
      .tmux_binary = "/opt/libtmux/custom-tmux",
      .socket_path = "/caller/must-not-select-this",
      .session_name = "caller-must-not-select-this",
      .startup_timeout = std::chrono::milliseconds{311},
      .shutdown_timeout = std::chrono::milliseconds{733},
      .retained_reply_bytes = 12345U,
      .line_bytes = 4321U,
      .pane_output = true,
      .pause_after = std::chrono::seconds{17},
  };

  const auto routed = libtmux::detail::routed_control_options(
      std::move(requested), "/server/selected/socket", "selected-session");

  EXPECT_EQ(routed.tmux_binary, std::filesystem::path{"/opt/libtmux/custom-tmux"});
  EXPECT_EQ(routed.socket_path, std::filesystem::path{"/server/selected/socket"});
  EXPECT_EQ(routed.session_name, "selected-session");
  EXPECT_EQ(routed.startup_timeout, std::chrono::milliseconds{311});
  EXPECT_EQ(routed.shutdown_timeout, std::chrono::milliseconds{733});
  EXPECT_EQ(routed.retained_reply_bytes, 12345U);
  EXPECT_EQ(routed.line_bytes, 4321U);
  EXPECT_TRUE(routed.pane_output);
  EXPECT_EQ(routed.pause_after, std::chrono::seconds{17});
}

TEST(BackendSeam, ExpansionRejectsAReplyFromAnotherTarget) {
  const std::string separator{libtmux::kFormatSeparator};
  auto backend = std::make_shared<ScriptedBackend>(
      std::vector<std::string>{"%8" + separator + "wrong\n"});

  const auto expanded =
      libtmux::detail::expand_format(backend, "%7", "pane_id", "pane", "#{pane_title}");

  ASSERT_FALSE(expanded.has_value());
  EXPECT_EQ(expanded.error().kind, FailureKind::missing);
  EXPECT_TRUE(expanded.error().dispatched);
}

TEST(BackendSeam, ExpansionRemovesOnlyTheNewlineTmuxAdds) {
  const std::string separator{libtmux::kFormatSeparator};
  auto backend = std::make_shared<ScriptedBackend>(
      std::vector<std::string>{"%7" + separator + "kept\n\n"});

  const auto expanded =
      libtmux::detail::expand_format(backend, "%7", "pane_id", "pane", "#{pane_title}");

  ASSERT_TRUE(expanded.has_value()) << expanded.error().diagnostic;
  EXPECT_EQ(*expanded, "kept\n");
}

TEST(BackendSeam, SnapshotFormattingPrecedesTheCommandTerminator) {
  constexpr std::array<std::string_view, 1> fields{"session_id"};
  auto backend = std::make_shared<ScriptedBackend>(
      std::vector<std::string>{"$0" + std::string{libtmux::kFormatSeparator} + "\n"});

  const auto snapshot =
      libtmux::Snapshot::take(backend, fields, {"new-session", "-d", "--", "command"},
                              libtmux::FormatArgument::flag);

  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().diagnostic;
  const auto& command = backend->issued.front();
  const auto format = std::ranges::find(command, "-F");
  const auto terminator = std::ranges::find(command, "--");
  ASSERT_NE(format, command.end());
  ASSERT_NE(terminator, command.end());
  EXPECT_EQ(std::distance(format, terminator), 2);
}

TEST(BackendSeam, InvalidEnvironmentNamesFailBeforeDispatch) {
  for (const std::string& name : {std::string{}, std::string{"BAD=NAME"}}) {
    std::vector<std::string> command{"new-session"};
    const auto appended =
        libtmux::detail::append_environment(command, {{name, "value"}});
    ASSERT_FALSE(appended.has_value());
    EXPECT_FALSE(appended.error().dispatched);
    EXPECT_EQ(command, (std::vector<std::string>{"new-session"}));
  }
}

TEST(BackendSeam, UnreadableKeyTablesFailBeforeDispatch) {
  auto backend = std::make_shared<ScriptedBackend>(std::vector<std::string>{});
  const Server server = libtmux::detail::server_over(backend);

  const auto bound = server.bind_key("bad table", "x", {"display-message"});

  ASSERT_FALSE(bound.has_value());
  EXPECT_FALSE(bound.error().dispatched);
  EXPECT_TRUE(backend->issued.empty());
}

TEST(BackendSeam, BufferLoadingNamesTheDestination) {
  auto backend = std::make_shared<ScriptedBackend>(std::vector<std::string>{""});
  const Server server = libtmux::detail::server_over(backend);

  const auto loaded = server.load_buffer("named", "/tmp/libtmux-buffer");

  ASSERT_TRUE(loaded.has_value()) << loaded.error().diagnostic;
  ASSERT_EQ(backend->issued.size(), 1U);
  EXPECT_EQ(backend->issued.front(),
            (std::vector<std::string>{"load-buffer", "-b", "named", "--",
                                      "/tmp/libtmux-buffer"}));
}

TEST(BackendSeam, AControlBatchKeepsOneOperationPerCommand) {
  libtmux::CommandBatch batch;
  ASSERT_TRUE(batch.add({"display-message", "one"}));
  ASSERT_TRUE(batch.add({"display-message", "two"}));

  const libtmux::ControlRequest request = libtmux::detail::batch_request(batch);

  ASSERT_EQ(request.group.size(), 2U);
  EXPECT_EQ(request.group[0].argv,
            (std::vector<std::string>{"display-message", "one"}));
  EXPECT_EQ(request.group[1].argv,
            (std::vector<std::string>{"display-message", "two"}));
}

TEST(BackendSeam, NotificationRetentionDropsTheOldestAtItsBound) {
  constexpr std::size_t expected_bound = 4096U;
  std::vector<libtmux::Notification> notifications;
  std::size_t dropped = 0U;
  for (std::size_t index = 0U; index <= expected_bound; ++index) {
    libtmux::Notification notification;
    notification.body.push_back(static_cast<std::byte>(index & 0xffU));
    libtmux::detail::retain_notification(notifications, dropped,
                                         std::move(notification));
  }

  ASSERT_EQ(notifications.size(), expected_bound);
  EXPECT_EQ(dropped, 1U);
  ASSERT_FALSE(notifications.front().body.empty());
  EXPECT_EQ(notifications.front().body.front(), std::byte{1});
}

} // namespace
