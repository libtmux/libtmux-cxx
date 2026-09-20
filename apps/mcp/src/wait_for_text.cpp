#include "wait_for_text.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtmux/entities.hpp"
#include "libtmux/server.hpp"
#include "libtmux/snapshot.hpp"
#include "libtmux/wait.hpp"
#include "pane_echo.hpp"
#include "tool_support.hpp"

namespace libtmux::mcp::detail {
namespace {

[[nodiscard]] ToolError cancellation_error() {
  return ToolError{false, "request cancelled"};
}

// The wire spelling of how the wait was answered. `WaitPath` is the library's
// answer; these strings are this server's published schema, so the mapping is
// written out rather than derived.
[[nodiscard]] std::string wire_mode(const libtmux::WaitResult& result) {
  std::string mode;
  switch (result.path) {
  case libtmux::WaitPath::pane_lookup:
    mode = "pane-lookup";
    break;
  case libtmux::WaitPath::capture_at_entry:
    mode = "capture-at-entry";
    break;
  case libtmux::WaitPath::capture_after_control_connect:
    mode = "capture-after-control-connect";
    break;
  case libtmux::WaitPath::capture_before_control:
    mode = "capture-before-control";
    break;
  case libtmux::WaitPath::control_output:
    mode = "control-output";
    break;
  case libtmux::WaitPath::capture_polling:
    mode = "capture-polling";
    break;
  }
  // `wanted` is on screen but the pane did not produce it. Said out loud, so a
  // caller can tell this from a plain, silent timeout.
  if (result.present_unconfirmed) {
    mode += "-unconfirmed";
  }
  return mode;
}

// A wait that expires before its target resolves has no pane to name, and the
// output schema admits `pane_id` only as a real pane ID.
[[nodiscard]] ToolOutput wait_output(const libtmux::WaitResult& result,
                                     std::string_view mode, long long elapsed_ms,
                                     std::string_view pane_id) {
  StructuredValue::Object structured{
      {"elapsed_ms", StructuredValue{elapsed_ms}},
      {"matched", StructuredValue{result.matched}},
      {"matched_at_entry", StructuredValue{result.matched_at_entry}},
      {"mode", StructuredValue{std::string{mode}}},
      {"text", StructuredValue{result.text}},
      {"timed_out", StructuredValue{result.timed_out}}};
  if (!pane_id.empty()) {
    structured.emplace("pane_id", StructuredValue{std::string{pane_id}});
  }
  return output(std::move(structured));
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

// Use the same daemon generation as pane-input preflight.
[[nodiscard]] std::optional<PaneServerIdentity>
resolve_pane_server_identity(const Server& server, std::string_view target,
                             std::chrono::milliseconds timeout) {
  const auto reply = server.run({"display-message", "-p", "-t", std::string{target},
                                 "--", "#{pid} #{start_time}"},
                                timeout);
  if (!reply.has_value()) {
    return std::nullopt;
  }
  std::string_view text = *reply;
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  const auto separator = text.find(' ');
  if (separator == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view pid_field = text.substr(0, separator);
  const std::string_view start_field = text.substr(separator + 1);
  std::uint64_t pid = 0;
  std::uint64_t start_time = 0;
  const auto pid_parsed =
      std::from_chars(pid_field.data(), pid_field.data() + pid_field.size(), pid);
  const auto start_parsed = std::from_chars(
      start_field.data(), start_field.data() + start_field.size(), start_time);
  if (pid_parsed.ec != std::errc{} ||
      pid_parsed.ptr != pid_field.data() + pid_field.size() ||
      start_parsed.ec != std::errc{} ||
      start_parsed.ptr != start_field.data() + start_field.size() || pid == 0U ||
      start_time == 0U) {
    return std::nullopt;
  }
  return PaneServerIdentity{.socket_path = std::string{server.socket_path()},
                            .server_pid = pid,
                            .server_start_time = start_time};
}

} // namespace

ToolResult wait_for_text(const Server& server, const Arguments& arguments,
                         const CallContext& context) {
  const auto budget = std::chrono::milliseconds{timeout_budget(arguments)};
  const auto started = std::chrono::steady_clock::now();

  libtmux::WaitOptions options;
  options.timeout = budget;
  // Retain submitted echoes across TTL expiry, but re-read pending input.
  const auto identity_timeout =
      std::max(std::chrono::milliseconds{1},
               std::chrono::duration_cast<std::chrono::milliseconds>(
                   budget - (std::chrono::steady_clock::now() - started)));
  const std::optional<PaneServerIdentity> identity = resolve_pane_server_identity(
      server, *argument(arguments, "target"), identity_timeout);
  const auto seen_recent = std::make_shared<std::set<std::string>>();
  const auto input_pending = std::make_shared<bool>(false);
  options.input_pending = [input_pending](std::string_view) { return *input_pending; };
  options.sent = [identity, seen_recent,
                  input_pending](std::string_view pane_id) -> std::vector<std::string> {
    if (!identity.has_value()) {
      return {};
    }
    const LiveEcho echo = live_echo(*identity, pane_id);
    *input_pending = echo.input_pending;
    for (const std::string& line : echo.recent) {
      seen_recent->insert(line);
    }
    std::vector<std::string> discounted(seen_recent->begin(), seen_recent->end());
    if (!echo.pending.empty()) {
      discounted.push_back(echo.pending);
    }
    return discounted;
  };
  options.cancelled = [&context] { return context.cancelled(); };
  options.on_progress = [&context, budget](std::chrono::milliseconds elapsed,
                                           libtmux::WaitPath path) {
    const auto completed = std::min(elapsed, budget);
    context.report(static_cast<double>(completed.count()),
                   static_cast<double>(budget.count()),
                   "waiting via " + wire_mode(libtmux::WaitResult{.path = path}));
  };
  // While this process's own wait holds a control connection open,
  // `list_sessions`/`get_session_info` must not read that connection's client
  // as somebody attached — an agent deciding whether it is safe to act on a
  // session should not see its own observation as another user.
  options.observe_control_client = [](std::int64_t pid) -> std::shared_ptr<void> {
    return std::make_shared<OwnObservationClient>(pid);
  };

  options.timeout = std::max(std::chrono::milliseconds::zero(),
                             budget - std::chrono::ceil<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - started));
  auto waited = server.wait_for_text(*argument(arguments, "target"),
                                     *argument(arguments, "text"), options);
  if (!waited.has_value()) {
    if (waited.error().kind == FailureKind::cancelled) {
      return libtmux::unexpected(cancellation_error());
    }
    return libtmux::unexpected(tmux_error(waited.error()));
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  return wait_output(*waited, wire_mode(*waited), elapsed.count(), waited->pane_id);
}

} // namespace libtmux::mcp::detail
