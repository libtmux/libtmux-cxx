#include "wait_for_text.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "libtmux/capture.hpp"
#include "libtmux/control.hpp"
#include "libtmux/server.hpp"
#include "libtmux/snapshot.hpp"
#include "pane_input.hpp"
#include "tool_support.hpp"

namespace libtmux::mcp::detail {
namespace {

struct WaitAnswer {
  bool matched{};
  bool matched_at_entry{};
  bool timed_out{};
  long long elapsed_ms{};
  std::string mode;
  std::string pane_id;
  std::string text;
};

class WaitDeadline {
public:
  explicit WaitDeadline(long long budget)
      : started_{std::chrono::steady_clock::now()},
        deadline_{started_ + std::chrono::milliseconds{budget}}, budget_{budget} {}

  [[nodiscard]] std::optional<std::chrono::milliseconds> remaining() const {
    const auto left = deadline_ - std::chrono::steady_clock::now();
    if (left <= std::chrono::steady_clock::duration::zero()) {
      return std::nullopt;
    }
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(left);
    if (milliseconds < left) {
      milliseconds += std::chrono::milliseconds{1};
    }
    return std::max(milliseconds, std::chrono::milliseconds{1});
  }

  [[nodiscard]] bool expired() const noexcept {
    return std::chrono::steady_clock::now() >= deadline_;
  }

  [[nodiscard]] long long elapsed() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - started_)
        .count();
  }

  [[nodiscard]] long long budget() const noexcept { return budget_; }
  [[nodiscard]] std::chrono::steady_clock::time_point started() const noexcept {
    return started_;
  }
  [[nodiscard]] std::chrono::steady_clock::time_point deadline() const noexcept {
    return deadline_;
  }

private:
  std::chrono::steady_clock::time_point started_;
  std::chrono::steady_clock::time_point deadline_;
  long long budget_;
};

struct WaitTarget {
  std::string pane_id;
  std::string session_name;
};

using WaitCommandResult = libtmux::expected<std::optional<std::string>, ToolError>;

[[nodiscard]] ToolError cancellation_error() {
  return ToolError{false, "request cancelled"};
}

// Whether every occurrence of `wanted` in a capture is confined to its
// final, non-blank row. `capture-pane -p` pads its output to the pane's
// full height, so the "last line" a caller's own just-submitted command
// still occupies — echoed there before the shell has run it and moved on
// — is the last row that is not blank padding, not the literal last byte.
// The library owns the hard part: telling output apart from a command still
// on the prompt and from this server's own echo. `libtmux::output_confirms`
// is that check, and every consumer waiting on pane text needs it.
[[nodiscard]] bool confirmed_by_output(std::string_view text, std::string_view wanted,
                                       const std::vector<std::string>& pending_input) {
  return libtmux::output_confirms(text, wanted, pending_input);
}

// Every caller below defers a match confined to the active row, or confined
// to text this server itself typed, rather than reporting it — treating it
// exactly like "no match yet" and falling through to the next capture. That
// alone would let a deferred match still be sitting in `text` when the
// deadline finally passes. This is where every timeout is built, and a
// deferred match still standing at the deadline is never promoted to
// `matched`: a match whose only occurrence is text this server itself sent
// is not a match, however new the bytes are. `mode`
// still gets an "-unconfirmed" suffix when `wanted` is present but
// unconfirmed, so the caller can tell "I saw something, but could not
// credit it to the pane" apart from a plain, silent timeout.
[[nodiscard]] WaitAnswer
wait_timed_out(const WaitDeadline& deadline, std::string_view wanted, std::string mode,
               std::string pane_id = {}, std::string text = {},
               bool matched_at_entry = {},
               const std::vector<std::string>& pending_input = {}) {
  if (!wanted.empty() && confirmed_by_output(text, wanted, pending_input)) {
    return WaitAnswer{.matched = true,
                      .matched_at_entry = matched_at_entry,
                      .elapsed_ms = deadline.elapsed(),
                      .mode = std::move(mode),
                      .pane_id = std::move(pane_id),
                      .text = std::move(text)};
  }
  const bool still_pending = !wanted.empty() && text.find(wanted) != std::string::npos;
  return WaitAnswer{.matched_at_entry = matched_at_entry,
                    .timed_out = true,
                    .elapsed_ms = deadline.elapsed(),
                    .mode = still_pending ? mode + "-unconfirmed" : std::move(mode),
                    .pane_id = std::move(pane_id),
                    .text = std::move(text)};
}

[[nodiscard]] WaitCommandResult run_before_deadline(const Server& server,
                                                    std::vector<std::string> command,
                                                    const WaitDeadline& deadline,
                                                    const CallContext& context) {
  if (context.cancelled()) {
    return libtmux::unexpected(cancellation_error());
  }
  const auto remaining = deadline.remaining();
  if (!remaining.has_value()) {
    return std::optional<std::string>{};
  }
  auto reply = server.run(command, *remaining);
  if (!reply.has_value()) {
    if (reply.error().kind == FailureKind::timeout) {
      return std::optional<std::string>{};
    }
    return libtmux::unexpected(tmux_error(reply.error()));
  }
  if (context.cancelled()) {
    return libtmux::unexpected(cancellation_error());
  }
  return std::optional<std::string>{*std::move(reply)};
}

[[nodiscard]] libtmux::expected<std::optional<WaitTarget>, ToolError>
resolve_wait_target(const Server& server, std::string_view target,
                    const WaitDeadline& deadline, const CallContext& context) {
  constexpr std::array fields{std::string_view{"pane_id"},
                              std::string_view{"session_name"}};
  auto reply = run_before_deadline(server,
                                   {"display-message", "-p", "-t", std::string{target},
                                    "--", format_request(fields)},
                                   deadline, context);
  if (!reply.has_value()) {
    return libtmux::unexpected(reply.error());
  }
  if (!reply->has_value()) {
    return std::optional<WaitTarget>{};
  }
  auto snapshot = Snapshot::from_recording(fields, *std::move(*reply));
  if (snapshot == nullptr || snapshot->rows().size() != 1U ||
      snapshot->rows().front()[0].empty() || snapshot->rows().front()[1].empty()) {
    return libtmux::unexpected(
        ToolError{false, "tmux could not resolve the requested pane"});
  }
  return std::optional<WaitTarget>{
      WaitTarget{.pane_id = std::string{snapshot->rows().front()[0]},
                 .session_name = std::string{snapshot->rows().front()[1]}}};
}

[[nodiscard]] WaitCommandResult capture_before_deadline(const Server& server,
                                                        const WaitTarget& target,
                                                        const WaitDeadline& deadline,
                                                        const CallContext& context) {
  // `-J`: without it, tmux reports a soft-wrapped line — one logical line
  // that ran past the pane's width — as separate lines split at the wrap
  // column, with no marker that the break is not a real one. A `wanted`
  // string straddling that column then never matches, in both the search
  // below and matches_only_the_active_row's, because the two say the same
  // thing about a promise this capture makes: what looks like one row on
  // screen is one line in the text searched for it.
  return run_before_deadline(server, {"capture-pane", "-p", "-J", "-t", target.pane_id},
                             deadline, context);
}

// A wait that expires before its target resolves has no pane to name, and the
// output schema admits `pane_id` only as a real pane ID.
[[nodiscard]] ToolOutput wait_output(WaitAnswer answer) {
  StructuredValue::Object structured{
      {"elapsed_ms", StructuredValue{answer.elapsed_ms}},
      {"matched", StructuredValue{answer.matched}},
      {"matched_at_entry", StructuredValue{answer.matched_at_entry}},
      {"mode", StructuredValue{std::move(answer.mode)}},
      {"text", StructuredValue{std::move(answer.text)}},
      {"timed_out", StructuredValue{answer.timed_out}}};
  if (!answer.pane_id.empty()) {
    structured.emplace("pane_id", StructuredValue{std::move(answer.pane_id)});
  }
  return output(std::move(structured));
}

void report_wait_progress(const CallContext& context, const WaitDeadline& deadline,
                          std::string_view mode) {
  const auto completed = std::min(deadline.elapsed(), deadline.budget());
  context.report(static_cast<double>(completed), static_cast<double>(deadline.budget()),
                 "waiting via " + std::string{mode});
}

[[nodiscard]] libtmux::expected<bool, ToolError>
bounded_contains(std::string_view text, std::string_view wanted,
                 std::size_t& remaining_work) {
  if (text.size() > remaining_work || wanted.size() > remaining_work - text.size()) {
    return libtmux::unexpected(ToolError{false, "wait matching work limit exceeded"});
  }
  remaining_work -= text.size() + wanted.size();
  return text.find(wanted) != std::string_view::npos;
}

[[nodiscard]] libtmux::expected<WaitAnswer, ToolError>
poll_for_text(const Server& server, const WaitTarget& target, std::string_view wanted,
              const CallContext& context, const WaitDeadline& deadline,
              std::size_t& remaining_match_work, std::string mode,
              const std::vector<std::string>& pending_input, bool matched_at_entry,
              std::string last = {}) {
  auto next_progress = deadline.started();
  while (!deadline.expired()) {
    if (context.cancelled()) {
      return libtmux::unexpected(cancellation_error());
    }
    auto captured = capture_before_deadline(server, target, deadline, context);
    if (!captured.has_value()) {
      return libtmux::unexpected(captured.error());
    }
    if (!captured->has_value()) {
      return wait_timed_out(deadline, wanted, std::move(mode), target.pane_id,
                            std::move(last), matched_at_entry, pending_input);
    }
    last = *std::move(*captured);
    if (deadline.expired()) {
      return wait_timed_out(deadline, wanted, std::move(mode), target.pane_id,
                            std::move(last), matched_at_entry, pending_input);
    }
    const auto matched = bounded_contains(last, wanted, remaining_match_work);
    if (!matched.has_value()) {
      return libtmux::unexpected(matched.error());
    }
    if (*matched && confirmed_by_output(last, wanted, pending_input)) {
      return WaitAnswer{.matched = true,
                        .matched_at_entry = matched_at_entry,
                        .elapsed_ms = deadline.elapsed(),
                        .mode = std::move(mode),
                        .pane_id = target.pane_id,
                        .text = std::move(last)};
    }
    if (std::chrono::steady_clock::now() >= next_progress) {
      report_wait_progress(context, deadline, mode);
      next_progress = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    }
    const auto remaining = deadline.remaining();
    if (remaining.has_value()) {
      std::this_thread::sleep_for(std::min(*remaining, std::chrono::milliseconds{50}));
    }
  }
  return wait_timed_out(deadline, wanted, std::move(mode), target.pane_id,
                        std::move(last), matched_at_entry, pending_input);
}

[[nodiscard]] libtmux::expected<WaitAnswer, ToolError>
stream_for_text(const Server& server, const WaitTarget& target, std::string_view wanted,
                const CallContext& context, const WaitDeadline& deadline,
                std::size_t& remaining_match_work, std::string initial_capture,
                const std::vector<std::string>& pending_input, bool matched_at_entry) {
  if (context.cancelled()) {
    return libtmux::unexpected(cancellation_error());
  }
  // Not `#{socket_path}`: tmux escapes non-printable bytes in the socket path
  // when it stores it at server start, so a socket named with one comes back as
  // its `\376` spelling and names no file. Connecting to that fails, and this
  // would fall back to polling without ever saying why. The handle already
  // knows the exact bytes every command here travels over.
  const std::string_view socket = server.socket_path();
  if (socket.empty()) {
    return poll_for_text(server, target, wanted, context, deadline,
                         remaining_match_work, "capture-polling", pending_input,
                         matched_at_entry, std::move(initial_capture));
  }
  const auto remaining = deadline.remaining();
  if (!remaining.has_value()) {
    return wait_timed_out(deadline, wanted, "capture-before-control", target.pane_id,
                          std::move(initial_capture), matched_at_entry, pending_input);
  }

  ConnectionOptions options;
  options.socket_path = std::string{socket};
  options.session_name = target.session_name;
  options.startup_timeout = std::min(*remaining, std::chrono::milliseconds{2000});
  options.shutdown_timeout = std::min(*remaining, std::chrono::milliseconds{500});
  options.pane_output = true;
  options.pause_after = std::chrono::seconds{2};
  auto connected = Connection::connect(std::move(options));
  if (!connected.has_value()) {
    return poll_for_text(server, target, wanted, context, deadline,
                         remaining_match_work, "capture-polling", pending_input,
                         matched_at_entry, std::move(initial_capture));
  }

  Connection connection = *std::move(connected);
  // This connection's own control client must not read as an attached
  // client to list_sessions/get_session_info for as long as it stays open
  // below.
  const OwnObservationClient own_client{connection.native_child_pid()};
  if (context.cancelled()) {
    return libtmux::unexpected(cancellation_error());
  }
  auto after_connect = capture_before_deadline(server, target, deadline, context);
  if (!after_connect.has_value()) {
    return libtmux::unexpected(after_connect.error());
  }
  if (!after_connect->has_value()) {
    return wait_timed_out(deadline, wanted, "capture-after-control-connect",
                          target.pane_id, std::move(initial_capture), matched_at_entry,
                          pending_input);
  }
  initial_capture = *std::move(*after_connect);
  if (deadline.expired()) {
    return wait_timed_out(deadline, wanted, "capture-after-control-connect",
                          target.pane_id, std::move(initial_capture), matched_at_entry,
                          pending_input);
  }
  const auto matched_after_connect =
      bounded_contains(initial_capture, wanted, remaining_match_work);
  if (!matched_after_connect.has_value()) {
    return libtmux::unexpected(matched_after_connect.error());
  }
  if (*matched_after_connect &&
      confirmed_by_output(initial_capture, wanted, pending_input)) {
    return WaitAnswer{.matched = true,
                      .matched_at_entry = matched_at_entry,
                      .elapsed_ms = deadline.elapsed(),
                      .mode = "capture-after-control-connect",
                      .pane_id = target.pane_id,
                      .text = std::move(initial_capture)};
  }

  auto next_progress = deadline.started();
  while (!deadline.expired()) {
    if (context.cancelled()) {
      return libtmux::unexpected(cancellation_error());
    }
    const auto remaining_slice = deadline.remaining();
    if (!remaining_slice.has_value()) {
      break;
    }
    const auto slice_deadline =
        std::chrono::steady_clock::now() +
        std::min(*remaining_slice, std::chrono::milliseconds{50});
    auto notifications = connection.wait_for_notifications(slice_deadline);
    if (notifications.empty()) {
      if (std::chrono::steady_clock::now() + std::chrono::milliseconds{5} <
          slice_deadline) {
        return poll_for_text(server, target, wanted, context, deadline,
                             remaining_match_work, "capture-polling", pending_input,
                             matched_at_entry, std::move(initial_capture));
      }
    }
    for (const Notification& notification : notifications) {
      const ParsedNotification parsed = parse(notification);
      if ((parsed.kind != NotificationKind::output &&
           parsed.kind != NotificationKind::extended_output) ||
          parsed.pane != target.pane_id) {
        continue;
      }
      auto captured = capture_before_deadline(server, target, deadline, context);
      if (!captured.has_value()) {
        return libtmux::unexpected(captured.error());
      }
      if (!captured->has_value()) {
        return wait_timed_out(deadline, wanted, "control-output", target.pane_id,
                              std::move(initial_capture), matched_at_entry,
                              pending_input);
      }
      initial_capture = *std::move(*captured);
      if (deadline.expired()) {
        return wait_timed_out(deadline, wanted, "control-output", target.pane_id,
                              std::move(initial_capture), matched_at_entry,
                              pending_input);
      }
      const auto matched =
          bounded_contains(initial_capture, wanted, remaining_match_work);
      if (!matched.has_value()) {
        return libtmux::unexpected(matched.error());
      }
      if (*matched && confirmed_by_output(initial_capture, wanted, pending_input)) {
        return WaitAnswer{.matched = true,
                          .matched_at_entry = matched_at_entry,
                          .elapsed_ms = deadline.elapsed(),
                          .mode = "control-output",
                          .pane_id = target.pane_id,
                          .text = std::move(initial_capture)};
      }
    }
    if (std::chrono::steady_clock::now() >= next_progress) {
      report_wait_progress(context, deadline, "control-output");
      next_progress = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    }
  }
  return wait_timed_out(deadline, wanted, "control-output", target.pane_id,
                        std::move(initial_capture), matched_at_entry, pending_input);
}

[[nodiscard]] long long timeout_budget(const Arguments& arguments) {
  const std::string* const supplied = argument(arguments, "timeout_ms");
  if (supplied == nullptr) {
    return 10000;
  }
  long long budget = 0;
  const char* const end = supplied->data() + supplied->size();
  const auto [stopped, code] = std::from_chars(supplied->data(), end, budget);
  if (code != std::errc{} || stopped != end) {
    return 10000;
  }
  return budget;
}

} // namespace

ToolResult wait_for_text(const Server& server, const Arguments& arguments,
                         const CallContext& context) {
  const WaitDeadline deadline{timeout_budget(arguments)};
  auto target =
      resolve_wait_target(server, *argument(arguments, "target"), deadline, context);
  if (!target.has_value()) {
    return libtmux::unexpected(target.error());
  }
  const std::string& wanted = *argument(arguments, "text");
  std::size_t remaining_match_work = 8U * 1024U * 1024U;
  if (!target->has_value()) {
    return wait_output(wait_timed_out(deadline, wanted, "pane-lookup"));
  }
  const std::vector<std::string> pending_input =
      remembered_pane_input(server.socket_path(), (*target)->pane_id);
  auto captured = capture_before_deadline(server, **target, deadline, context);
  if (!captured.has_value()) {
    return libtmux::unexpected(captured.error());
  }
  if (!captured->has_value()) {
    return wait_output(
        wait_timed_out(deadline, wanted, "capture-at-entry", (*target)->pane_id));
  }
  std::string initial_capture = *std::move(*captured);
  // The screen at entry is read and reported as its own outcome before
  // anything else runs, so a later capture that merely
  // rediscovers text already here at the start is never confused with fresh
  // output.
  const auto matched = bounded_contains(initial_capture, wanted, remaining_match_work);
  if (!matched.has_value()) {
    return libtmux::unexpected(matched.error());
  }
  const bool matched_at_entry = *matched;
  if (deadline.expired()) {
    return wait_output(wait_timed_out(deadline, wanted, "capture-at-entry",
                                      (*target)->pane_id, std::move(initial_capture),
                                      matched_at_entry, pending_input));
  }
  // A match confined to the pane's active row, or confined to text this
  // server itself typed, is the caller's own not-yet-run input echoed back
  // — see confirmed_by_output.
  if (*matched && confirmed_by_output(initial_capture, wanted, pending_input)) {
    return wait_output(WaitAnswer{.matched = true,
                                  .matched_at_entry = matched_at_entry,
                                  .elapsed_ms = deadline.elapsed(),
                                  .mode = "capture-at-entry",
                                  .pane_id = (*target)->pane_id,
                                  .text = std::move(initial_capture)});
  }
  auto answer =
      stream_for_text(server, **target, wanted, context, deadline, remaining_match_work,
                      std::move(initial_capture), pending_input, matched_at_entry);
  if (!answer.has_value()) {
    return libtmux::unexpected(answer.error());
  }
  return wait_output(*std::move(answer));
}

} // namespace libtmux::mcp::detail
