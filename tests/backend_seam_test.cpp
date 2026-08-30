// The transport seam, exercised rather than asserted.
//
// The library talks to tmux through one private interface. This test supplies
// a different implementation of it — one that launches nothing and answers
// from a script — and drives the whole public surface over it. That proves two
// things at once: an async executor can be dropped in without
// touching an installed header, and the exact argv every operation sends,
// which a test against a live server can only observe indirectly.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <semaphore>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "libtmux/entities.hpp"
#include "libtmux/format.hpp"
#include "libtmux/server.hpp"

#include "acquire.hpp"
#include "backend.hpp"
#include "notification_stream.hpp"

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
      : ScriptedBackend{std::move(replies),
                        libtmux::Version{.major = 3, .minor = 7, .revision = 2}} {}

  ScriptedBackend(std::vector<std::string> replies, libtmux::Version version,
                  libtmux::ExecutionPolicy policy = {})
      : Backend{{}, policy}, replies_{std::move(replies)}, version_{version} {}

  // Stands in for an implementation, so a refusal only Windows would meet is
  // reachable here. Unrecognised by default, which is what a custom executor
  // reports.
  libtmux::ServerCapabilities declared{};
  [[nodiscard]] libtmux::ServerCapabilities capabilities() const noexcept override {
    return declared;
  }

  expected<std::string, CommandFailure>
  run(const libtmux::CommandRequest& command,
      std::optional<std::chrono::milliseconds> timeout,
      std::optional<std::size_t> output_limit) const override {
    issued.push_back(command.argv());
    command_timeouts.push_back(timeout);
    command_output_limits.push_back(output_limit);
    if (gate_run) {
      run_started.release();
      continue_run.acquire();
    }
    std::this_thread::sleep_for(delay);
    if (replies_.empty()) {
      return unexpected(CommandFailure{.kind = FailureKind::refused,
                                       .delivery = libtmux::DeliveryStatus::replied,
                                       .exit_code = 1,
                                       .diagnostic = "the script ran out"});
    }
    std::string reply = replies_.front();
    replies_.erase(replies_.begin());
    return reply;
  }

  CommandFailure report(CommandFailure failure,
                        const libtmux::CommandRequest& command) const {
    return report_failure(command, std::move(failure)).error();
  }

  expected<std::string, CommandFailure>
  run_batch(const libtmux::CommandBatch& batch,
            std::optional<std::chrono::milliseconds> timeout,
            std::optional<std::size_t> output_limit) const override {
    std::vector<std::vector<std::string>> commands;
    commands.reserve(batch.commands().size());
    for (const libtmux::CommandRequest& command : batch.commands()) {
      commands.push_back(command.argv());
    }
    batches.push_back(std::move(commands));
    return run(batch.request(), timeout, output_limit);
  }

  const std::vector<std::string>& connection() const noexcept override {
    return connection_;
  }

  expected<libtmux::Version, CommandFailure> version() const override {
    ++version_queries;
    version_timeouts.push_back(policy().timeout);
    version_output_limits.push_back(policy().output_limit);
    std::this_thread::sleep_for(delay);
    return version_;
  }

  mutable std::vector<std::vector<std::string>> issued;
  mutable std::vector<std::vector<std::vector<std::string>>> batches;
  mutable std::vector<std::optional<std::chrono::milliseconds>> command_timeouts;
  mutable std::vector<std::optional<std::size_t>> command_output_limits;
  mutable std::vector<std::optional<std::chrono::milliseconds>> version_timeouts;
  mutable std::vector<std::optional<std::size_t>> version_output_limits;
  mutable std::size_t version_queries{};
  std::chrono::milliseconds delay{};
  bool gate_run{false};
  mutable std::binary_semaphore run_started{0};
  mutable std::binary_semaphore continue_run{0};

private:
  mutable std::vector<std::string> replies_;
  libtmux::Version version_;
  std::vector<std::string> connection_{"-S", "/scripted"};
};

template <std::size_t Size>
std::string entity_row(const std::array<std::string_view, Size>& values) {
  std::string output;
  for (const std::string_view value : values) {
    output += value;
    output += libtmux::kFormatSeparator;
  }
  output += '\n';
  return output;
}

// One row of session fields, in the order the entity asks for them.
std::string session_row(std::string_view id, std::string_view name) {
  const std::string separator{libtmux::kFormatSeparator};
  return std::string{id} + separator + std::string{name} + separator + "1" + separator +
         "2" + separator + "/tmp" + separator + "1700000000" + separator + separator +
         "0" + separator + "\n";
}

std::string pane_row(std::string_view id, std::string_view window_id,
                     std::string_view session_id) {
  return entity_row(std::array<std::string_view, libtmux::Pane::kFields.size()>{
      id, "sh", "0", window_id, session_id, "1", "", "123", "/dev/pts/1", "/tmp", "80",
      "24", "0", "0", "1", "1", "1", "1", "0"});
}

std::string window_row(std::string_view id, std::string_view name,
                       std::string_view session_id) {
  return entity_row(std::array<std::string_view, libtmux::Window::kFields.size()>{
      id, name, "0", session_id, "2", "1", "80", "24", "", "0", "0", "0", "1"});
}

std::string named_window_row(std::string_view id, std::string_view name,
                             std::string_view session_id, std::string_view version,
                             std::string_view automatic_rename) {
  return entity_row(std::array<std::string_view, libtmux::Window::kFields.size() + 2U>{
      id, name, "0", session_id, "2", "1", "80", "24", "", "0", "0", "0", "1", version,
      automatic_rename});
}

std::string octal_word(std::string_view value) {
  std::string quoted{"\""};
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    quoted.push_back('\\');
    quoted.push_back(static_cast<char>('0' + ((byte >> 6U) & 7U)));
    quoted.push_back(static_cast<char>('0' + ((byte >> 3U) & 7U)));
    quoted.push_back(static_cast<char>('0' + (byte & 7U)));
  }
  quoted.push_back('"');
  return quoted;
}

std::vector<std::byte> bytes(std::string_view text) {
  std::vector<std::byte> result;
  result.reserve(text.size());
  for (const char byte : text) {
    result.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
  }
  return result;
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

TEST(BackendSeam, SubmissionReturnsBeforeAFallbackBackendAnswers) {
  auto backend = std::make_shared<ScriptedBackend>(std::vector<std::string>{"later"});
  backend->delay = std::chrono::milliseconds{300};
  const Server server = libtmux::detail::server_over(backend);

  const auto started = std::chrono::steady_clock::now();
  auto submitted = server.submit({"display-message", "-p", "later"});
  const auto submit_took = std::chrono::steady_clock::now() - started;

  ASSERT_TRUE(submitted.has_value()) << submitted.error().diagnostic;
  EXPECT_LT(submit_took, std::chrono::milliseconds{100});
  auto answer = std::move(*submitted).wait();
  ASSERT_TRUE(answer.has_value()) << answer.error().diagnostic;
  EXPECT_EQ(*answer, "later");
}

TEST(BackendSeam, DroppingAnOperationDoesNotWaitForItsFallbackBackend) {
  auto backend = std::make_shared<ScriptedBackend>(std::vector<std::string>{"later"});
  backend->gate_run = true;
  const Server server = libtmux::detail::server_over(backend);

  auto submitted = server.submit({"display-message", "-p", "later"});
  ASSERT_TRUE(submitted.has_value()) << submitted.error().diagnostic;
  ASSERT_TRUE(backend->run_started.try_acquire_for(std::chrono::seconds{1}));

  std::binary_semaphore dropped{0};
  std::thread dropper{[operation = std::move(*submitted), &dropped]() mutable {
    std::optional<libtmux::CommandOperation> held{std::move(operation)};
    held.reset();
    dropped.release();
  }};
  const bool returned = dropped.try_acquire_for(std::chrono::seconds{1});
  backend->continue_run.release();
  dropper.join();

  EXPECT_TRUE(returned);
}

TEST(BackendSeam, QueuedFallbackCancellationPreventsDispatch) {
  auto first_backend =
      std::make_shared<ScriptedBackend>(std::vector<std::string>{"first"});
  auto second_backend =
      std::make_shared<ScriptedBackend>(std::vector<std::string>{"second"});
  auto cancelled_backend =
      std::make_shared<ScriptedBackend>(std::vector<std::string>{"must not run"});
  first_backend->delay = std::chrono::milliseconds{300};
  second_backend->delay = std::chrono::milliseconds{300};
  const Server first_server = libtmux::detail::server_over(first_backend);
  const Server second_server = libtmux::detail::server_over(second_backend);
  const Server cancelled_server = libtmux::detail::server_over(cancelled_backend);

  auto first = first_server.submit({"display-message", "-p", "first"});
  auto second = second_server.submit({"display-message", "-p", "second"});
  auto cancelled = cancelled_server.submit({"display-message", "-p", "must not run"});
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(cancelled.has_value());
  EXPECT_TRUE(cancelled->request_cancel());

  EXPECT_TRUE(std::move(*first).wait().has_value());
  EXPECT_TRUE(std::move(*second).wait().has_value());
  auto answer = std::move(*cancelled).wait();

  ASSERT_FALSE(answer.has_value());
  EXPECT_EQ(answer.error().kind, FailureKind::cancelled);
  EXPECT_EQ(answer.error().delivery, libtmux::DeliveryStatus::not_started);
  EXPECT_TRUE(cancelled_backend->issued.empty());
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

// psmux refuses what it cannot target safely, and until now that answer only
// existed in a Windows build. Declaring the implementation reaches it here.
TEST(BackendSeam, APsmuxServerRefusesNavigationWithoutDispatching) {
  auto backend = std::make_shared<ScriptedBackend>(
      std::vector<std::string>{session_row("$7", "work"), ""});
  backend->declared = {.implementation = libtmux::ServerImplementation::psmux,
                       .backend = libtmux::BackendKind::subprocess};
  const Server server = libtmux::detail::server_over(backend);
  const auto sessions = server.sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  const auto listed = backend->issued.size();

  const auto moved = sessions->front().select_next_window();

  ASSERT_FALSE(moved.has_value());
  EXPECT_EQ(moved.error().kind, libtmux::FailureKind::unsupported);
  EXPECT_EQ(moved.error().delivery, libtmux::DeliveryStatus::not_started);
  // Nothing ran, so there is no status to report: the exit code every other
  // refusal in this library carries.
  EXPECT_EQ(moved.error().exit_code, 0);
  EXPECT_NE(moved.error().diagnostic.find("session navigation"), std::string::npos);
  EXPECT_EQ(backend->issued.size(), listed) << "refused after dispatching";
}

// The server-scoped surface refuses the same way, and says which state it
// cannot provide rather than failing at the wire.
TEST(BackendSeam, APsmuxServerRefusesServerScopedState) {
  auto backend = std::make_shared<ScriptedBackend>(std::vector<std::string>{});
  backend->declared = {.implementation = libtmux::ServerImplementation::psmux,
                       .backend = libtmux::BackendKind::subprocess};
  const Server server = libtmux::detail::server_over(backend);

  const auto clients = server.clients();

  ASSERT_FALSE(clients.has_value());
  EXPECT_EQ(clients.error().kind, libtmux::FailureKind::unsupported);
  EXPECT_EQ(clients.error().delivery, libtmux::DeliveryStatus::not_started);
  EXPECT_EQ(clients.error().exit_code, 0);
  EXPECT_NE(clients.error().diagnostic.find("clients"), std::string::npos);
  EXPECT_TRUE(backend->issued.empty()) << "refused after dispatching";
}

// The same call over a backend nobody recognises still runs: unfamiliar is not
// the same as known-broken.
TEST(BackendSeam, AnUnrecognisedBackendIsNotRefused) {
  auto backend = std::make_shared<ScriptedBackend>(
      std::vector<std::string>{session_row("$7", "work"), "", ""});
  const Server server = libtmux::detail::server_over(backend);
  const auto sessions = server.sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  const auto listed = backend->issued.size();

  static_cast<void>(sessions->front().select_next_window());

  EXPECT_GT(backend->issued.size(), listed);
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

TEST(BackendSeam, RawTmux37RepairsABrokenOutWindowByStableId) {
  auto backend = std::make_shared<ScriptedBackend>(std::vector<std::string>{
      pane_row("%7", "@3", "$2"), named_window_row("@9", "sh", "$2", "3.7", "1"),
      named_window_row("@9", "roomy", "$2", "3.7", "0")});
  const auto snapshot = libtmux::Snapshot::take(
      backend, libtmux::Pane::kFields, {"list-panes"}, libtmux::FormatArgument::flag);
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().diagnostic;
  const libtmux::Pane pane{*snapshot, 0};

  const auto broken = pane.break_out("roomy");

  ASSERT_TRUE(broken.has_value()) << broken.error().diagnostic;
  EXPECT_EQ(broken->id(), "@9");
  EXPECT_EQ(broken->name(), "roomy");
  EXPECT_EQ(backend->version_queries, 0U);
  ASSERT_EQ(backend->issued.size(), 3U);
  ASSERT_EQ(backend->issued[1].size(), 7U);
  EXPECT_EQ(std::vector(backend->issued[1].begin(), backend->issued[1].begin() + 5),
            (std::vector<std::string>{"if-shell", "-F", "-t", "%7",
                                      "#{==:#{window_panes},1}"}));
  EXPECT_TRUE(backend->issued[1][6].starts_with("break-pane -d -s %7 -t $2: -P -n " +
                                                octal_word("roomy")));
  EXPECT_EQ(backend->issued[1][6].find("roomy"), std::string::npos);
  ASSERT_EQ(backend->batches.size(), 1U);
  ASSERT_EQ(backend->batches.front().size(), 3U);
  const auto& guard = backend->batches.front()[0];
  EXPECT_EQ(guard.front(), "if-shell");
  EXPECT_EQ(guard.back(), "{");
  EXPECT_NE(guard[4].find("after-rename-window"), std::string::npos);
  EXPECT_NE(guard[4].find("window-renamed"), std::string::npos);
  EXPECT_NE(guard[4].find("after-display-message"), std::string::npos);
  EXPECT_EQ(backend->batches.front()[1],
            (std::vector<std::string>{"rename-window", "-t", "$2:@9", "--", "roomy"}));
  EXPECT_EQ(backend->batches.front()[2].front(), "display-message");
}

TEST(BackendSeam, RawTmux37NameRepairTreatsHashesLiterally) {
  constexpr std::string_view requested = "#{session_name}#,},comma";
  auto backend = std::make_shared<ScriptedBackend>(std::vector<std::string>{
      pane_row("%7", "@3", "$2"), named_window_row("@9", "sh", "$2", "3.7", "1"),
      named_window_row("@9", requested, "$2", "3.7", "0")});
  const auto snapshot = libtmux::Snapshot::take(
      backend, libtmux::Pane::kFields, {"list-panes"}, libtmux::FormatArgument::flag);
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().diagnostic;
  const libtmux::Pane pane{*snapshot, 0};

  const auto broken = pane.break_out(requested);

  ASSERT_TRUE(broken.has_value()) << broken.error().diagnostic;
  EXPECT_EQ(broken->name(), requested);
  ASSERT_EQ(backend->issued[1].size(), 7U);
  EXPECT_NE(backend->issued[1][5].find(octal_word(libtmux::escape_literal(requested))),
            std::string::npos);
  EXPECT_NE(backend->issued[1][6].find(octal_word(requested)), std::string::npos);
  EXPECT_EQ(backend->issued[1][5].find(requested), std::string::npos);
  EXPECT_EQ(backend->issued[1][6].find(requested), std::string::npos);
  ASSERT_EQ(backend->batches.size(), 1U);
  EXPECT_EQ(backend->batches.front()[1],
            (std::vector<std::string>{"rename-window", "-t", "$2:@9", "--",
                                      "##{session_name}##,},comma"}));
}

TEST(BackendSeam, RawTmux37RepairsACoincidentNaturalName) {
  auto backend = std::make_shared<ScriptedBackend>(std::vector<std::string>{
      pane_row("%7", "@3", "$2"), named_window_row("@9", "sh", "$2", "3.7", "1"),
      named_window_row("@9", "sh", "$2", "3.7", "0")});
  const auto snapshot = libtmux::Snapshot::take(
      backend, libtmux::Pane::kFields, {"list-panes"}, libtmux::FormatArgument::flag);
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().diagnostic;
  const libtmux::Pane pane{*snapshot, 0};

  const auto broken = pane.break_out("sh");

  ASSERT_TRUE(broken.has_value()) << broken.error().diagnostic;
  EXPECT_EQ(broken->name(), "sh");
  ASSERT_EQ(backend->batches.size(), 1U);
  EXPECT_EQ(backend->batches.front()[1].back(), "sh");
}

TEST(BackendSeam, RawTmux37OnePaneKeepsItsNativeCleanedName) {
  auto backend = std::make_shared<ScriptedBackend>(std::vector<std::string>{
      pane_row("%7", "@3", "$2"),
      named_window_row("@3", R"(back\\slash\t雪)", "$2", "3.7", "0")});
  const auto snapshot = libtmux::Snapshot::take(
      backend, libtmux::Pane::kFields, {"list-panes"}, libtmux::FormatArgument::flag);
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().diagnostic;
  const libtmux::Pane pane{*snapshot, 0};

  const auto broken = pane.break_out("back\\slash\t雪");

  ASSERT_TRUE(broken.has_value()) << broken.error().diagnostic;
  EXPECT_EQ(broken->name(), R"(back\\slash\t雪)");
  EXPECT_TRUE(backend->batches.empty());
  EXPECT_EQ(backend->issued.size(), 2U);
}

TEST(BackendSeam, RawTmux37RepairReturnsTmuxsCleanedName) {
  auto backend = std::make_shared<ScriptedBackend>(std::vector<std::string>{
      pane_row("%7", "@3", "$2"), named_window_row("@9", "sh", "$2", "3.7", "1"),
      named_window_row("@9", R"(back\\slash\t雪)", "$2", "3.7", "0")});
  const auto snapshot = libtmux::Snapshot::take(
      backend, libtmux::Pane::kFields, {"list-panes"}, libtmux::FormatArgument::flag);
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().diagnostic;
  const libtmux::Pane pane{*snapshot, 0};

  const auto broken = pane.break_out("back\\slash\t雪");

  ASSERT_TRUE(broken.has_value()) << broken.error().diagnostic;
  EXPECT_EQ(broken->name(), R"(back\\slash\t雪)");
  EXPECT_EQ(backend->batches.size(), 1U);
}

TEST(BackendSeam, RawTmux37RepairSharesOneDeadline) {
  const libtmux::ExecutionPolicy policy{.timeout = std::chrono::milliseconds{100},
                                        .output_limit = 777U};
  auto backend = std::make_shared<ScriptedBackend>(
      std::vector<std::string>{pane_row("%7", "@3", "$2"),
                               named_window_row("@9", "sh", "$2", "3.7", "1"),
                               named_window_row("@9", "roomy", "$2", "3.7", "0")},
      libtmux::Version{.major = 3, .minor = 7}, policy);
  backend->delay = std::chrono::milliseconds{5};
  const auto snapshot = libtmux::Snapshot::take(
      backend, libtmux::Pane::kFields, {"list-panes"}, libtmux::FormatArgument::flag);
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().diagnostic;
  const libtmux::Pane pane{*snapshot, 0};

  const auto broken = pane.break_out("roomy");

  ASSERT_TRUE(broken.has_value()) << broken.error().diagnostic;
  ASSERT_EQ(backend->command_timeouts.size(), 3U);
  for (std::size_t index = 1U; index < backend->command_timeouts.size(); ++index) {
    ASSERT_TRUE(backend->command_timeouts[index].has_value());
    EXPECT_EQ(backend->command_output_limits[index], 777U);
  }
  EXPECT_GT(*backend->command_timeouts[1], *backend->command_timeouts[2]);
}

TEST(BackendSeam, RawTmux37NameRepairReportsTheMovedPane) {
  auto backend = std::make_shared<ScriptedBackend>(std::vector<std::string>{
      pane_row("%7", "@3", "$2"), named_window_row("@9", "sh", "$2", "3.7", "1")});
  const auto snapshot = libtmux::Snapshot::take(
      backend, libtmux::Pane::kFields, {"list-panes"}, libtmux::FormatArgument::flag);
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().diagnostic;
  const libtmux::Pane pane{*snapshot, 0};

  const auto broken = pane.break_out("roomy");

  ASSERT_FALSE(broken.has_value());
  EXPECT_EQ(broken.error().kind, FailureKind::refused);
  EXPECT_NE(broken.error().delivery, libtmux::DeliveryStatus::not_started);
  EXPECT_NE(broken.error().diagnostic.find("moved pane %7 into window @9"),
            std::string::npos)
      << broken.error().diagnostic;
  EXPECT_NE(broken.error().diagnostic.find(
                "raw tmux 3.7 name repair failed: the script ran out"),
            std::string::npos)
      << broken.error().diagnostic;
  ASSERT_EQ(backend->issued.size(), 3U);
  ASSERT_EQ(backend->batches.size(), 1U);
}

TEST(BackendSeam, NamedBreakMismatchOnAnotherVersionIsReturnedUnchanged) {
  auto backend = std::make_shared<ScriptedBackend>(
      std::vector<std::string>{pane_row("%7", "@3", "$2"),
                               named_window_row("@9", "unexpected", "$2", "3.7a", "0")},
      libtmux::Version{.major = 3, .minor = 7, .revision = 1});
  const auto snapshot = libtmux::Snapshot::take(
      backend, libtmux::Pane::kFields, {"list-panes"}, libtmux::FormatArgument::flag);
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().diagnostic;
  const libtmux::Pane pane{*snapshot, 0};

  const auto broken = pane.break_out("roomy");

  ASSERT_TRUE(broken.has_value()) << broken.error().diagnostic;
  EXPECT_EQ(broken->id(), "@9");
  EXPECT_EQ(broken->name(), "unexpected");
  EXPECT_EQ(backend->version_queries, 0U);
  EXPECT_EQ(backend->issued.size(), 2U);
  EXPECT_TRUE(backend->batches.empty());
}

TEST(BackendSeam, NamedBreakRejectsAReplyFromAnotherSession) {
  auto backend = std::make_shared<ScriptedBackend>(
      std::vector<std::string>{pane_row("%7", "@3", "$2"),
                               named_window_row("@3", "hooked", "$4", "3.7", "0")},
      libtmux::Version{.major = 3, .minor = 7});
  const auto snapshot = libtmux::Snapshot::take(
      backend, libtmux::Pane::kFields, {"list-panes"}, libtmux::FormatArgument::flag);
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().diagnostic;
  const libtmux::Pane pane{*snapshot, 0};

  const auto broken = pane.break_out("roomy");

  ASSERT_FALSE(broken.has_value());
  EXPECT_EQ(broken.error().kind, FailureKind::refused);
  EXPECT_NE(broken.error().delivery, libtmux::DeliveryStatus::not_started);
  EXPECT_NE(broken.error().diagnostic.find("exact connected window"), std::string::npos)
      << broken.error().diagnostic;
  EXPECT_EQ(backend->version_queries, 0U);
  EXPECT_EQ(backend->issued.size(), 2U);
  EXPECT_TRUE(backend->batches.empty());
}

TEST(BackendSeam, EveryOtherTmuxKeepsTheNativeNamedBreakPath) {
  constexpr std::array versions{
      std::string_view{"3.6"},
      std::string_view{"3.7a"},
      std::string_view{"3.7b"},
      std::string_view{"3.8"},
  };
  for (const std::string_view version : versions) {
    SCOPED_TRACE(version);
    auto backend = std::make_shared<ScriptedBackend>(
        std::vector<std::string>{pane_row("%7", "@3", "$2"),
                                 named_window_row("@9", "roomy", "$2", version, "0")});
    const auto snapshot = libtmux::Snapshot::take(
        backend, libtmux::Pane::kFields, {"list-panes"}, libtmux::FormatArgument::flag);
    ASSERT_TRUE(snapshot.has_value()) << snapshot.error().diagnostic;
    const libtmux::Pane pane{*snapshot, 0};

    const auto broken = pane.break_out("roomy");

    ASSERT_TRUE(broken.has_value()) << broken.error().diagnostic;
    EXPECT_EQ(broken->name(), "roomy");
    EXPECT_EQ(backend->version_queries, 0U);
    ASSERT_EQ(backend->issued.size(), 2U);
    ASSERT_EQ(backend->issued[1].size(), 7U);
    EXPECT_EQ(backend->issued[1][4], "#{==:#{window_panes},1}");
    EXPECT_TRUE(backend->issued[1][6].starts_with("break-pane -d -s %7 -t $2: -P -n " +
                                                  octal_word("roomy")));
    EXPECT_TRUE(backend->batches.empty());
  }
}

TEST(BackendSeam, NamedBreakRejectsMalformedPostMutationMetadata) {
  for (const auto& [version, automatic_rename] :
       {std::pair{std::string_view{"not-a-version"}, std::string_view{"0"}},
        std::pair{std::string_view{"3.7"}, std::string_view{"yes"}}}) {
    SCOPED_TRACE(::testing::Message() << version << '/' << automatic_rename);
    auto backend = std::make_shared<ScriptedBackend>(std::vector<std::string>{
        pane_row("%7", "@3", "$2"),
        named_window_row("@9", "roomy", "$2", version, automatic_rename)});
    const auto snapshot = libtmux::Snapshot::take(
        backend, libtmux::Pane::kFields, {"list-panes"}, libtmux::FormatArgument::flag);
    ASSERT_TRUE(snapshot.has_value()) << snapshot.error().diagnostic;
    const libtmux::Pane pane{*snapshot, 0};

    const auto broken = pane.break_out("roomy");

    ASSERT_FALSE(broken.has_value());
    EXPECT_EQ(broken.error().kind, FailureKind::refused);
    EXPECT_NE(broken.error().delivery, libtmux::DeliveryStatus::not_started);
    EXPECT_NE(broken.error().diagnostic.find("version or automatic-rename"),
              std::string::npos)
        << broken.error().diagnostic;
  }
}

TEST(BackendSeam, RawTmux37RepairRequiresTheSameWindowAndDurableNamePolicy) {
  for (const std::string& final : {
           named_window_row("@10", "roomy", "$2", "3.7", "0"),
           named_window_row("@9", "roomy", "$3", "3.7", "0"),
           named_window_row("@9", "roomy", "$2", "3.7", "1"),
       }) {
    SCOPED_TRACE(final);
    auto backend = std::make_shared<ScriptedBackend>(std::vector<std::string>{
        pane_row("%7", "@3", "$2"), named_window_row("@9", "sh", "$2", "3.7", "1"),
        final});
    const auto snapshot = libtmux::Snapshot::take(
        backend, libtmux::Pane::kFields, {"list-panes"}, libtmux::FormatArgument::flag);
    ASSERT_TRUE(snapshot.has_value()) << snapshot.error().diagnostic;
    const libtmux::Pane pane{*snapshot, 0};

    const auto broken = pane.break_out("roomy");

    ASSERT_FALSE(broken.has_value());
    EXPECT_EQ(broken.error().kind, FailureKind::refused);
    EXPECT_NE(broken.error().delivery, libtmux::DeliveryStatus::not_started);
  }
}

TEST(BackendSeam, UnnamedBreakAtomicallyGuardsOnlyRawTmux37) {
  auto backend = std::make_shared<ScriptedBackend>(
      std::vector<std::string>{pane_row("%7", "@3", "$2"),
                               window_row("@9", "sh", "$2")},
      libtmux::Version{.major = 3, .minor = 7});
  const auto snapshot = libtmux::Snapshot::take(
      backend, libtmux::Pane::kFields, {"list-panes"}, libtmux::FormatArgument::flag);
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().diagnostic;
  const libtmux::Pane pane{*snapshot, 0};

  const auto broken = pane.break_out();

  ASSERT_TRUE(broken.has_value()) << broken.error().diagnostic;
  EXPECT_EQ(broken->name(), "sh");
  EXPECT_EQ(backend->version_queries, 0U);
  ASSERT_EQ(backend->issued.size(), 2U);
  const std::string reported = "break-pane -d -s %7 -t $2: -P -F '" +
                               libtmux::format_request(libtmux::Window::kFields) + "'";
  const std::string current = "display-message -p -t $2:@3 '" +
                              libtmux::format_request(libtmux::Window::kFields) + "'";
  const std::string guarded = "if-shell -F -t %7 '#{==:#{version},3.7}' { " + reported +
                              " -n libtmux } { " + reported + " }";
  EXPECT_EQ(backend->issued[1],
            (std::vector<std::string>{"if-shell", "-F", "-t", "%7",
                                      "#{==:#{window_panes},1}", current, guarded}));
}

TEST(BackendSeam, AFailedInsertedBreakHasNoMaskingFollowup) {
  auto backend = std::make_shared<ScriptedBackend>(
      std::vector<std::string>{pane_row("%7", "@3", "$2")},
      libtmux::Version{.major = 3, .minor = 7});
  const auto snapshot = libtmux::Snapshot::take(
      backend, libtmux::Pane::kFields, {"list-panes"}, libtmux::FormatArgument::flag);
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().diagnostic;
  const libtmux::Pane pane{*snapshot, 0};

  const auto broken = pane.break_out();

  ASSERT_FALSE(broken.has_value());
  EXPECT_EQ(broken.error().kind, FailureKind::refused);
  EXPECT_NE(broken.error().delivery, libtmux::DeliveryStatus::not_started);
  EXPECT_EQ(broken.error().diagnostic, "the script ran out");
  EXPECT_EQ(backend->issued.size(), 2U);
}

TEST(BackendSeam, UnnamedBreakRejectsMalformedOrNoncanonicalWindowRows) {
  for (const std::string& reply :
       {std::string{"$2:@9\n"}, window_row("@09", "sh", "$2"),
        window_row("@9", "sh", "$02"), window_row("@4294967296", "sh", "$2"),
        window_row("@9", "sh", "$4294967296"), window_row("@9", "sh", "$3"),
        window_row("@9", "sh", "$2") + window_row("@10", "sh", "$2")}) {
    SCOPED_TRACE(reply);
    auto backend = std::make_shared<ScriptedBackend>(
        std::vector<std::string>{pane_row("%7", "@3", "$2"), reply},
        libtmux::Version{.major = 3, .minor = 7});
    const auto snapshot = libtmux::Snapshot::take(
        backend, libtmux::Pane::kFields, {"list-panes"}, libtmux::FormatArgument::flag);
    ASSERT_TRUE(snapshot.has_value()) << snapshot.error().diagnostic;
    const libtmux::Pane pane{*snapshot, 0};

    const auto broken = pane.break_out();

    ASSERT_FALSE(broken.has_value());
    EXPECT_EQ(broken.error().kind, FailureKind::refused);
    EXPECT_NE(broken.error().delivery, libtmux::DeliveryStatus::not_started);
    EXPECT_NE(broken.error().diagnostic.find("break-pane completed for pane %7"),
              std::string::npos)
        << broken.error().diagnostic;
    EXPECT_EQ(backend->issued.size(), 2U);
  }
}

TEST(BackendSeam, UnnamedBreakPreservesMissingForAReportWithoutOneWindow) {
  auto backend = std::make_shared<ScriptedBackend>(
      std::vector<std::string>{pane_row("%7", "@3", "$2"), ""});
  const auto snapshot = libtmux::Snapshot::take(
      backend, libtmux::Pane::kFields, {"list-panes"}, libtmux::FormatArgument::flag);
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().diagnostic;
  const libtmux::Pane pane{*snapshot, 0};

  const auto broken = pane.break_out();

  ASSERT_FALSE(broken.has_value());
  EXPECT_EQ(broken.error().kind, FailureKind::missing);
  EXPECT_NE(broken.error().delivery, libtmux::DeliveryStatus::not_started);
  EXPECT_EQ(broken.error().diagnostic, "tmux has no window %7");
  EXPECT_EQ(backend->issued.size(), 2U);
}

TEST(BackendSeam, UnnamedBreakDecodesItsConnectedWindowRow) {
  const std::string name = "left" + std::string{libtmux::kFormatSeparator} + "middle" +
                           std::string{libtmux::kFormatEscape} + "right";
  const std::string encoded = "left" + std::string{libtmux::kFormatEscape} + "Smiddle" +
                              std::string{libtmux::kFormatEscape} + "Eright";
  auto backend = std::make_shared<ScriptedBackend>(std::vector<std::string>{
      pane_row("%7", "@3", "$2"), window_row("@9", encoded, "$2")});
  const auto snapshot = libtmux::Snapshot::take(
      backend, libtmux::Pane::kFields, {"list-panes"}, libtmux::FormatArgument::flag);
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().diagnostic;
  const libtmux::Pane pane{*snapshot, 0};

  const auto broken = pane.break_out();

  ASSERT_TRUE(broken.has_value()) << broken.error().diagnostic;
  EXPECT_EQ(broken->name(), name);
  EXPECT_TRUE(broken->server().has_value());
  EXPECT_EQ(backend->issued.size(), 2U);
}

TEST(BackendSeam, RawTmux37RejectsAnUnsafeIdBeforeBuildingACommandString) {
  for (const std::string_view id : {"%00", "%4294967296", "%7; kill-server"}) {
    SCOPED_TRACE(id);
    auto backend = std::make_shared<ScriptedBackend>(
        std::vector<std::string>{pane_row(id, "@3", "$2")},
        libtmux::Version{.major = 3, .minor = 7});
    const auto snapshot = libtmux::Snapshot::take(
        backend, libtmux::Pane::kFields, {"list-panes"}, libtmux::FormatArgument::flag);
    ASSERT_TRUE(snapshot.has_value()) << snapshot.error().diagnostic;
    const libtmux::Pane pane{*snapshot, 0};

    const auto broken = pane.break_out();

    ASSERT_FALSE(broken.has_value());
    EXPECT_EQ(broken.error().kind, FailureKind::validation);
    EXPECT_EQ(broken.error().delivery, libtmux::DeliveryStatus::not_started);
    EXPECT_NE(broken.error().diagnostic.find("stable numeric pane id"),
              std::string::npos)
        << broken.error().diagnostic;
    EXPECT_EQ(backend->issued.size(), 1U);
  }
}

TEST(BackendSeam, BreakOutRejectsAnUnsafeSessionBeforeBuildingACommandString) {
  for (const std::string_view name : {std::string_view{}, std::string_view{"roomy"}}) {
    SCOPED_TRACE(name);
    auto backend = std::make_shared<ScriptedBackend>(
        std::vector<std::string>{pane_row("%7", "@3", "$2; kill-server")});
    const auto snapshot = libtmux::Snapshot::take(
        backend, libtmux::Pane::kFields, {"list-panes"}, libtmux::FormatArgument::flag);
    ASSERT_TRUE(snapshot.has_value()) << snapshot.error().diagnostic;
    const libtmux::Pane pane{*snapshot, 0};

    const auto broken = pane.break_out(name);

    ASSERT_FALSE(broken.has_value());
    EXPECT_EQ(broken.error().kind, FailureKind::validation);
    EXPECT_EQ(broken.error().delivery, libtmux::DeliveryStatus::not_started);
    EXPECT_NE(broken.error().diagnostic.find("stable numeric session id"),
              std::string::npos)
        << broken.error().diagnostic;
    EXPECT_EQ(backend->issued.size(), 1U);
  }
}

TEST(BackendSeam, BreakOutRejectsAnUnsafeWindowBeforeBuildingACommandString) {
  for (const std::string_view name : {std::string_view{}, std::string_view{"roomy"}}) {
    SCOPED_TRACE(name);
    auto backend = std::make_shared<ScriptedBackend>(
        std::vector<std::string>{pane_row("%7", "@3; kill-server", "$2")});
    const auto snapshot = libtmux::Snapshot::take(
        backend, libtmux::Pane::kFields, {"list-panes"}, libtmux::FormatArgument::flag);
    ASSERT_TRUE(snapshot.has_value()) << snapshot.error().diagnostic;
    const libtmux::Pane pane{*snapshot, 0};

    const auto broken = pane.break_out(name);

    ASSERT_FALSE(broken.has_value());
    EXPECT_EQ(broken.error().kind, FailureKind::validation);
    EXPECT_EQ(broken.error().delivery, libtmux::DeliveryStatus::not_started);
    EXPECT_NE(broken.error().diagnostic.find("stable numeric window id"),
              std::string::npos)
        << broken.error().diagnostic;
    EXPECT_EQ(backend->issued.size(), 1U);
  }
}

TEST(BackendSeam, NamedBreakRejectsNulBeforeBuildingACommandString) {
  auto backend = std::make_shared<ScriptedBackend>(
      std::vector<std::string>{pane_row("%7", "@3", "$2")});
  const auto snapshot = libtmux::Snapshot::take(
      backend, libtmux::Pane::kFields, {"list-panes"}, libtmux::FormatArgument::flag);
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().diagnostic;
  const libtmux::Pane pane{*snapshot, 0};
  const std::string name{"room\0y", 6U};

  const auto broken = pane.break_out(name);

  ASSERT_FALSE(broken.has_value());
  EXPECT_EQ(broken.error().kind, FailureKind::validation);
  EXPECT_EQ(broken.error().delivery, libtmux::DeliveryStatus::not_started);
  EXPECT_NE(broken.error().diagnostic.find("cannot contain NUL"), std::string::npos)
      << broken.error().diagnostic;
  EXPECT_EQ(backend->issued.size(), 1U);
}

TEST(BackendSeam, Tmux37aUnnamedBreakLeavesNamingToTmux) {
  auto backend = std::make_shared<ScriptedBackend>(
      std::vector<std::string>{pane_row("%7", "@3", "$2"),
                               window_row("@9", "sh", "$2")},
      libtmux::Version{.major = 3, .minor = 7, .revision = 1});
  const auto snapshot = libtmux::Snapshot::take(
      backend, libtmux::Pane::kFields, {"list-panes"}, libtmux::FormatArgument::flag);
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().diagnostic;
  const libtmux::Pane pane{*snapshot, 0};

  const auto broken = pane.break_out();

  ASSERT_TRUE(broken.has_value()) << broken.error().diagnostic;
  EXPECT_EQ(broken->name(), "sh");
  EXPECT_EQ(backend->version_queries, 0U);
  ASSERT_EQ(backend->issued.size(), 2U);
  EXPECT_EQ(backend->issued[1].front(), "if-shell");
  const std::string native = "break-pane -d -s %7 -t $2: -P -F '" +
                             libtmux::format_request(libtmux::Window::kFields) + "'";
  EXPECT_EQ(backend->issued[1].back(), "if-shell -F -t %7 '#{==:#{version},3.7}' { " +
                                           native + " -n libtmux } { " + native + " }");
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
  EXPECT_NE(alive.error().delivery, libtmux::DeliveryStatus::not_started);
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
  EXPECT_EQ(observed_commands, 0U);
}

TEST(BackendSeam, VersionDetectionInheritsTheBackendPolicy) {
  const libtmux::ExecutionPolicy policy{.timeout = std::chrono::milliseconds{123},
                                        .output_limit = 456U};
  auto backend = std::make_shared<ScriptedBackend>(
      std::vector<std::string>{}, libtmux::Version{.major = 3, .minor = 8}, policy);
  const Server server = libtmux::detail::server_over(backend);

  const auto version = server.tmux_version();

  ASSERT_TRUE(version.has_value()) << version.error().diagnostic;
  EXPECT_EQ(*version, (libtmux::Version{.major = 3, .minor = 8}));
  ASSERT_EQ(backend->version_timeouts.size(), 1U);
  EXPECT_EQ(backend->version_timeouts.front(), std::chrono::milliseconds{123});
  ASSERT_EQ(backend->version_output_limits.size(), 1U);
  EXPECT_EQ(backend->version_output_limits.front(), 456U);
}

TEST(BackendSeam, ExpansionRejectsAReplyFromAnotherTarget) {
  const std::string separator{libtmux::kFormatSeparator};
  auto backend = std::make_shared<ScriptedBackend>(
      std::vector<std::string>{"%8" + separator + "wrong\n"});

  const auto expanded =
      libtmux::detail::expand_format(backend, "%7", "pane_id", "pane", "#{pane_title}");

  ASSERT_FALSE(expanded.has_value());
  EXPECT_EQ(expanded.error().kind, FailureKind::missing);
  EXPECT_NE(expanded.error().delivery, libtmux::DeliveryStatus::not_started);
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
    libtmux::CommandRequest command{"new-session"};
    const auto appended =
        libtmux::detail::append_environment(command, {{name, "value"}});
    ASSERT_FALSE(appended.has_value());
    EXPECT_EQ(appended.error().delivery, libtmux::DeliveryStatus::not_started);
    EXPECT_EQ(command.argv(), (std::vector<std::string>{"new-session"}));
  }
}

TEST(BackendSeam, ACompositeArgumentRedactsItsSensitivePart) {
  auto backend = std::make_shared<ScriptedBackend>(std::vector<std::string>{});
  const std::string secret{"sensitive-part-only"};
  libtmux::CommandRequest command{"new-session", "-e"};
  constexpr std::string_view prefix{"PUBLIC_NAME="};
  command.push_back(libtmux::CommandArgument::sensitive_range(
      std::string{prefix} + secret, prefix.size(), secret.size()));
  libtmux::CommandRequest copied = command;
  libtmux::CommandRequest moved = std::move(copied);

  const auto scrubbed = backend->report(
      CommandFailure{.diagnostic = "tmux rejected value " + secret}, moved);

  EXPECT_EQ(scrubbed.diagnostic.find(secret), std::string::npos) << scrubbed.diagnostic;
  EXPECT_NE(scrubbed.diagnostic.find("[REDACTED]"), std::string::npos)
      << scrubbed.diagnostic;
  const std::string rendered = libtmux::detail::rendered_command(moved);
  EXPECT_EQ(rendered.find(secret), std::string::npos) << rendered;
  EXPECT_NE(rendered.find("PUBLIC_NAME="), std::string::npos) << rendered;
}

TEST(BackendSeam, ASecretCannotReappearThroughTheRedactionMarker) {
  auto backend = std::make_shared<ScriptedBackend>(std::vector<std::string>{});
  const std::string secret{"[REDACTED]"};
  libtmux::CommandRequest command{"display-message"};
  command.push_back(libtmux::CommandArgument::sensitive(secret));

  const auto failure =
      backend->report(CommandFailure{.diagnostic = "tmux repeated " + secret}, command);
  const std::string rendered = libtmux::detail::rendered_command(command);

  EXPECT_EQ(failure.diagnostic.find(secret), std::string::npos) << failure.diagnostic;
  EXPECT_EQ(rendered.find(secret), std::string::npos) << rendered;
}

TEST(BackendSeam, AnInvalidEmptySensitiveRangeFailsClosed) {
  libtmux::CommandRequest command{"display-message"};
  command.push_back(
      libtmux::CommandArgument::sensitive_range("must-stay-private", 99U, 0U));

  const std::string rendered = libtmux::detail::rendered_command(command);

  EXPECT_EQ(rendered.find("must-stay-private"), std::string::npos) << rendered;
}

TEST(BackendSeam, UnreadableKeyTablesFailBeforeDispatch) {
  auto backend = std::make_shared<ScriptedBackend>(std::vector<std::string>{});
  const Server server = libtmux::detail::server_over(backend);

  const auto bound = server.bind_key("bad table", "x", {"display-message"});

  ASSERT_FALSE(bound.has_value());
  EXPECT_EQ(bound.error().delivery, libtmux::DeliveryStatus::not_started);
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

TEST(BackendSeam, NotificationRetentionDropsTheOldestAtItsBound) {
  constexpr std::size_t expected_bound = 4096U;
  libtmux::detail::NotificationStream notifications;
  const auto cursor = notifications.subscribe(true);
  for (std::size_t index = 0U; index <= expected_bound; ++index) {
    libtmux::Notification notification;
    notification.body.push_back(static_cast<std::byte>(index & 0xffU));
    notifications.push(std::move(notification));
  }

  EXPECT_EQ(notifications.dropped(cursor), 1U);
  auto held = notifications.take(cursor);
  ASSERT_EQ(held.size(), expected_bound);
  ASSERT_FALSE(held.front().body.empty());
  EXPECT_EQ(held.front().body.front(), std::byte{1});
  EXPECT_TRUE(notifications.take(cursor).empty());
}

TEST(BackendSeam, NotificationRetentionAlsoBoundsBytes) {
  libtmux::detail::NotificationStream notifications{8U, 5U};
  const auto cursor = notifications.subscribe(true);
  notifications.push(libtmux::Notification{.body = bytes("old")});
  notifications.push(libtmux::Notification{.body = bytes("new")});

  auto held = notifications.take(cursor);

  ASSERT_EQ(held.size(), 1U);
  EXPECT_EQ(held.front().body, bytes("new"));
  EXPECT_EQ(notifications.dropped(cursor), 1U);

  notifications.push(libtmux::Notification{.body = bytes("oversized")});
  EXPECT_TRUE(notifications.take(cursor).empty());
  EXPECT_EQ(notifications.dropped(cursor), 2U);
}

TEST(BackendSeam, NotificationDropsBelongToTheCursorThatFellBehind) {
  libtmux::detail::NotificationStream notifications{2U, 1024U};
  const auto fast = notifications.subscribe(true);
  const auto slow = notifications.subscribe(true);

  notifications.push(libtmux::Notification{.body = bytes("one")});
  notifications.push(libtmux::Notification{.body = bytes("two")});
  ASSERT_EQ(notifications.take(fast).size(), 2U);

  notifications.push(libtmux::Notification{.body = bytes("three")});
  const auto fast_tail = notifications.take(fast);
  const auto slow_tail = notifications.take(slow);

  ASSERT_EQ(fast_tail.size(), 1U);
  EXPECT_EQ(fast_tail.front().body, bytes("three"));
  EXPECT_EQ(notifications.dropped(fast), 0U);
  ASSERT_EQ(slow_tail.size(), 2U);
  EXPECT_EQ(slow_tail.front().body, bytes("two"));
  EXPECT_EQ(notifications.dropped(slow), 1U);
}

} // namespace
