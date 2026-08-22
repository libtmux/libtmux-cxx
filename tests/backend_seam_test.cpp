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
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
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
      : ScriptedBackend{std::move(replies),
                        libtmux::Version{.major = 3, .minor = 7, .revision = 2}} {}

  ScriptedBackend(std::vector<std::string> replies, libtmux::Version version,
                  libtmux::ExecutionPolicy policy = {})
      : Backend{{}, policy}, replies_{std::move(replies)}, version_{version} {}

  expected<std::string, CommandFailure>
  run(const std::vector<std::string>& command,
      std::optional<std::chrono::milliseconds> timeout,
      std::optional<std::size_t> output_limit) const override {
    issued.push_back(command);
    command_timeouts.push_back(timeout);
    command_output_limits.push_back(output_limit);
    std::this_thread::sleep_for(delay);
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

  expected<std::string, CommandFailure>
  run_batch(const libtmux::CommandBatch& batch,
            std::optional<std::chrono::milliseconds> timeout,
            std::optional<std::size_t> output_limit) const override {
    batches.push_back(batch.commands());
    return run(batch.argv(), timeout, output_limit);
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

std::vector<std::byte> bytes(std::string_view text) {
  std::vector<std::byte> result;
  result.reserve(text.size());
  for (const char byte : text) {
    result.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
  }
  return result;
}

libtmux::ControlBlock control_block(std::uint64_t sequence,
                                    libtmux::ControlTerminal terminal,
                                    std::string_view body = {}) {
  return {.sequence = sequence,
          .command_number = sequence,
          .terminal = terminal,
          .begin_metadata = {},
          .terminal_metadata = {},
          .body = bytes(body),
          .body_truncated = false,
          .body_bytes = body.size()};
}

libtmux::ControlRequestResult inserted_result(
    std::string_view wrapper_body, libtmux::ControlTerminal wrapper_terminal,
    std::string_view inserted_body, libtmux::ControlTerminal inserted_terminal) {
  libtmux::ControlRequestResult result;
  result.operations = {
      {.attribution = libtmux::Attribution::exact,
       .block = control_block(1U, wrapper_terminal, wrapper_body)},
      {.attribution = libtmux::Attribution::exact,
       .block = control_block(2U, inserted_terminal, inserted_body)},
  };
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
  ASSERT_EQ(backend->issued[1].size(), 9U);
  EXPECT_EQ(
      std::vector(backend->issued[1].begin(), backend->issued[1].begin() + 7),
      (std::vector<std::string>{"break-pane", "-d", "-s", "%7", "-P", "-n", "roomy"}));
  EXPECT_EQ(backend->issued[1][7], "-F");
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
      named_window_row("@3", R"(back\\slash\t雪)", "$4", "3.7", "0")});
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
  EXPECT_TRUE(broken.error().dispatched);
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

TEST(BackendSeam, NamedOnePaneMoveMismatchSkipsVersionDetection) {
  auto backend = std::make_shared<ScriptedBackend>(
      std::vector<std::string>{pane_row("%7", "@3", "$2"),
                               named_window_row("@3", "hooked", "$4", "3.7", "0")},
      libtmux::Version{.major = 3, .minor = 7});
  const auto snapshot = libtmux::Snapshot::take(
      backend, libtmux::Pane::kFields, {"list-panes"}, libtmux::FormatArgument::flag);
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().diagnostic;
  const libtmux::Pane pane{*snapshot, 0};

  const auto broken = pane.break_out("roomy");

  ASSERT_TRUE(broken.has_value()) << broken.error().diagnostic;
  EXPECT_EQ(broken->id(), "@3");
  EXPECT_EQ(broken->name(), "hooked");
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
    EXPECT_EQ(std::vector(backend->issued[1].begin(), backend->issued[1].begin() + 7),
              (std::vector<std::string>{"break-pane", "-d", "-s", "%7", "-P", "-n",
                                        "roomy"}));
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
    EXPECT_TRUE(broken.error().dispatched);
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
    EXPECT_TRUE(broken.error().dispatched);
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
  const std::string reported = "break-pane -d -s %7 -P -F '" +
                               libtmux::format_request(libtmux::Window::kFields) + "'";
  EXPECT_EQ(backend->issued[1],
            (std::vector<std::string>{"if-shell", "-F", "-t", "%7",
                                      "#{&&:#{==:#{version},3.7},"
                                      "#{>:#{window_panes},1}}",
                                      reported + " -n libtmux", reported}));
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
  EXPECT_TRUE(broken.error().dispatched);
  EXPECT_EQ(broken.error().diagnostic, "the script ran out");
  EXPECT_EQ(backend->issued.size(), 2U);
}

TEST(BackendSeam, UnnamedBreakRejectsMalformedOrNoncanonicalWindowRows) {
  for (const std::string& reply :
       {std::string{"$2:@9\n"}, window_row("@09", "sh", "$2"),
        window_row("@9", "sh", "$02"), window_row("@4294967296", "sh", "$2"),
        window_row("@9", "sh", "$4294967296"),
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
    EXPECT_TRUE(broken.error().dispatched);
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
  EXPECT_TRUE(broken.error().dispatched);
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
    EXPECT_FALSE(broken.error().dispatched);
    EXPECT_NE(broken.error().diagnostic.find("stable numeric pane id"),
              std::string::npos)
        << broken.error().diagnostic;
    EXPECT_EQ(backend->issued.size(), 1U);
  }
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
  EXPECT_EQ(backend->issued[1].back(),
            "break-pane -d -s %7 -P -F '" +
                libtmux::format_request(libtmux::Window::kFields) + "'");
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

TEST(BackendSeam, AnInsertedControlReplyReturnsOnlyTheSecondBlock) {
  const auto result = inserted_result({}, libtmux::ControlTerminal::end, "window-row\n",
                                      libtmux::ControlTerminal::end);

  const auto reply = libtmux::detail::inserted_command_reply(result, std::nullopt);

  ASSERT_TRUE(reply.has_value()) << reply.error().diagnostic;
  EXPECT_EQ(*reply, "window-row\n");
}

TEST(BackendSeam, AnInsertedControlFailureCannotBeMaskedByWrapperSuccess) {
  const auto result =
      inserted_result({}, libtmux::ControlTerminal::end, "break failed\n",
                      libtmux::ControlTerminal::error);

  const auto reply = libtmux::detail::inserted_command_reply(result, std::nullopt);

  ASSERT_FALSE(reply.has_value());
  EXPECT_EQ(reply.error().kind, FailureKind::refused);
  EXPECT_TRUE(reply.error().dispatched);
  EXPECT_EQ(reply.error().diagnostic, "break failed\n");

  const auto wrapper_failure =
      inserted_result("if-shell failed\n", libtmux::ControlTerminal::error, {},
                      libtmux::ControlTerminal::end);
  const auto wrapper_reply =
      libtmux::detail::inserted_command_reply(wrapper_failure, std::nullopt);
  ASSERT_FALSE(wrapper_reply.has_value());
  EXPECT_EQ(wrapper_reply.error().kind, FailureKind::refused);
  EXPECT_EQ(wrapper_reply.error().diagnostic, "if-shell failed\n");
}

TEST(BackendSeam, AnInsertedControlReplyChecksBothFramesAndTheCallBound) {
  auto wrapper_output = inserted_result("unexpected\n", libtmux::ControlTerminal::end,
                                        "row\n", libtmux::ControlTerminal::end);
  const auto rejected_wrapper =
      libtmux::detail::inserted_command_reply(wrapper_output, std::nullopt);
  ASSERT_FALSE(rejected_wrapper.has_value());
  EXPECT_EQ(rejected_wrapper.error().kind, FailureKind::pipe);

  auto truncated = inserted_result({}, libtmux::ControlTerminal::end, "row\n",
                                   libtmux::ControlTerminal::end);
  truncated.operations[1].block->body_truncated = true;
  truncated.operations[1].block->body_bytes = 100U;
  const auto rejected_capture =
      libtmux::detail::inserted_command_reply(truncated, std::nullopt);
  ASSERT_FALSE(rejected_capture.has_value());
  EXPECT_EQ(rejected_capture.error().kind, FailureKind::truncated);

  auto unattributed = inserted_result({}, libtmux::ControlTerminal::end, "row\n",
                                      libtmux::ControlTerminal::end);
  unattributed.operations[1].attribution = libtmux::Attribution::unknown;
  const auto rejected_attribution =
      libtmux::detail::inserted_command_reply(unattributed, std::nullopt);
  ASSERT_FALSE(rejected_attribution.has_value());
  EXPECT_EQ(rejected_attribution.error().kind, FailureKind::timeout);

  const auto bounded = inserted_result({}, libtmux::ControlTerminal::end, "row\n",
                                       libtmux::ControlTerminal::end);
  const auto rejected_bound = libtmux::detail::inserted_command_reply(bounded, 3U);
  ASSERT_FALSE(rejected_bound.has_value());
  EXPECT_EQ(rejected_bound.error().kind, FailureKind::truncated);
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
