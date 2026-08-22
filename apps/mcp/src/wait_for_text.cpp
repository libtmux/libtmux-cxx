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

#include "libtmux/control.hpp"
#include "libtmux/server.hpp"
#include "libtmux/snapshot.hpp"
#include "tool_support.hpp"

namespace libtmux::mcp::detail {
namespace {

struct WaitAnswer {
  bool matched{};
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

[[nodiscard]] std::string without_line_ending(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.pop_back();
  }
  return text;
}

[[nodiscard]] ToolError cancellation_error() {
  return ToolError{false, "request cancelled"};
}

[[nodiscard]] WaitAnswer wait_timed_out(const WaitDeadline& deadline, std::string mode,
                                        std::string pane_id = {},
                                        std::string text = {}) {
  return WaitAnswer{.timed_out = true,
                    .elapsed_ms = deadline.elapsed(),
                    .mode = std::move(mode),
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
  return run_before_deadline(server, {"capture-pane", "-p", "-t", target.pane_id},
                             deadline, context);
}

// A wait that expires before its target resolves has no pane to name, and the
// output schema admits `pane_id` only as a real pane ID.
[[nodiscard]] ToolOutput wait_output(WaitAnswer answer) {
  StructuredValue::Object structured{{"elapsed_ms", StructuredValue{answer.elapsed_ms}},
                                     {"matched", StructuredValue{answer.matched}},
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

[[nodiscard]] libtmux::expected<WaitAnswer, ToolError>
poll_for_text(const Server& server, const WaitTarget& target, std::string_view wanted,
              const CallContext& context, const WaitDeadline& deadline,
              std::string mode, std::string last = {}) {
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
      return wait_timed_out(deadline, std::move(mode), target.pane_id, std::move(last));
    }
    last = *std::move(*captured);
    if (deadline.expired()) {
      return wait_timed_out(deadline, std::move(mode), target.pane_id, std::move(last));
    }
    if (last.find(wanted) != std::string::npos) {
      return WaitAnswer{.matched = true,
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
  return wait_timed_out(deadline, std::move(mode), target.pane_id, std::move(last));
}

[[nodiscard]] libtmux::expected<WaitAnswer, ToolError>
stream_for_text(const Server& server, const WaitTarget& target, std::string_view wanted,
                const CallContext& context, const WaitDeadline& deadline,
                std::string initial_capture) {
  if (context.cancelled()) {
    return libtmux::unexpected(cancellation_error());
  }
  auto socket = run_before_deadline(
      server, {"display-message", "-p", "-t", target.pane_id, "--", "#{socket_path}"},
      deadline, context);
  if (!socket.has_value()) {
    if (context.cancelled()) {
      return libtmux::unexpected(socket.error());
    }
    return poll_for_text(server, target, wanted, context, deadline, "capture-polling",
                         std::move(initial_capture));
  }
  if (!socket->has_value()) {
    return wait_timed_out(deadline, "socket-path", target.pane_id,
                          std::move(initial_capture));
  }
  const auto remaining = deadline.remaining();
  if (!remaining.has_value()) {
    return wait_timed_out(deadline, "capture-before-control", target.pane_id,
                          std::move(initial_capture));
  }

  ConnectionOptions options;
  options.socket_path = without_line_ending(*std::move(*socket));
  options.session_name = target.session_name;
  options.startup_timeout = std::min(*remaining, std::chrono::milliseconds{2000});
  options.shutdown_timeout = std::min(*remaining, std::chrono::milliseconds{500});
  options.pane_output = true;
  options.pause_after = std::chrono::seconds{2};
  auto connected = Connection::connect(std::move(options));
  if (!connected.has_value()) {
    return poll_for_text(server, target, wanted, context, deadline, "capture-polling",
                         std::move(initial_capture));
  }

  Connection connection = *std::move(connected);
  if (context.cancelled()) {
    return libtmux::unexpected(cancellation_error());
  }
  auto after_connect = capture_before_deadline(server, target, deadline, context);
  if (!after_connect.has_value()) {
    return libtmux::unexpected(after_connect.error());
  }
  if (!after_connect->has_value()) {
    return wait_timed_out(deadline, "capture-after-control-connect", target.pane_id,
                          std::move(initial_capture));
  }
  initial_capture = *std::move(*after_connect);
  if (deadline.expired()) {
    return wait_timed_out(deadline, "capture-after-control-connect", target.pane_id,
                          std::move(initial_capture));
  }
  if (initial_capture.find(wanted) != std::string::npos) {
    return WaitAnswer{.matched = true,
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
                             "capture-polling", std::move(initial_capture));
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
        return wait_timed_out(deadline, "control-output", target.pane_id,
                              std::move(initial_capture));
      }
      initial_capture = *std::move(*captured);
      if (deadline.expired()) {
        return wait_timed_out(deadline, "control-output", target.pane_id,
                              std::move(initial_capture));
      }
      if (initial_capture.find(wanted) != std::string::npos) {
        return WaitAnswer{.matched = true,
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
  return wait_timed_out(deadline, "control-output", target.pane_id,
                        std::move(initial_capture));
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
  if (!target->has_value()) {
    return wait_output(wait_timed_out(deadline, "pane-lookup"));
  }
  auto captured = capture_before_deadline(server, **target, deadline, context);
  if (!captured.has_value()) {
    return libtmux::unexpected(captured.error());
  }
  if (!captured->has_value()) {
    return wait_output(
        wait_timed_out(deadline, "capture-at-entry", (*target)->pane_id));
  }
  std::string initial_capture = *std::move(*captured);
  if (deadline.expired()) {
    return wait_output(wait_timed_out(deadline, "capture-at-entry", (*target)->pane_id,
                                      std::move(initial_capture)));
  }
  if (initial_capture.find(wanted) != std::string::npos) {
    return wait_output(WaitAnswer{.matched = true,
                                  .elapsed_ms = deadline.elapsed(),
                                  .mode = "capture-at-entry",
                                  .pane_id = (*target)->pane_id,
                                  .text = std::move(initial_capture)});
  }
  auto answer = stream_for_text(server, **target, wanted, context, deadline,
                                std::move(initial_capture));
  if (!answer.has_value()) {
    return libtmux::unexpected(answer.error());
  }
  return wait_output(*std::move(answer));
}

} // namespace libtmux::mcp::detail
