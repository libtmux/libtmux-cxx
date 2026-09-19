#include "wait_for_text.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtmux/entities.hpp"
#include "libtmux/server.hpp"
#include "libtmux/snapshot.hpp"
#include "libtmux/wait.hpp"
#include "pane_input.hpp"
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

} // namespace

ToolResult wait_for_text(const Server& server, const Arguments& arguments,
                         const CallContext& context) {
  const auto budget = std::chrono::milliseconds{timeout_budget(arguments)};
  const auto started = std::chrono::steady_clock::now();

  libtmux::WaitOptions options;
  options.timeout = budget;
  // Everything this server has typed into that pane and not had confirmed:
  // without it the shell's echo of a command reads as the command's output.
  // Asked for by pane, which is why it is answered here rather than passed in
  // — the caller's target may name a session or a window.
  options.sent = [&server](std::string_view pane_id) {
    return remembered_pane_input(server.socket_path(), pane_id);
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

  auto waited = server.wait_for_text(*argument(arguments, "target"),
                                     *argument(arguments, "text"), std::move(options));
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
