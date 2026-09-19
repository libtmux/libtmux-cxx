#include "libtmux/wait.hpp"

#include "libtmux/capture.hpp"
#include "libtmux/control.hpp"
#include "libtmux/entities.hpp"
#include "libtmux/format.hpp"
#include "libtmux/notification.hpp"
#include "libtmux/server.hpp"
#include "libtmux/snapshot.hpp"
#include <array>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

LIBTMUX_NAMESPACE_BEGIN
namespace {

class Deadline {
public:
  explicit Deadline(std::chrono::milliseconds budget)
      : started_{std::chrono::steady_clock::now()}, deadline_{started_ + budget} {}

  // Absent once the budget is spent. Rounded up and floored at one
  // millisecond, so a caller with a sliver left still gets one real attempt
  // rather than a zero timeout, which every command here would read as
  // "give up now".
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

  [[nodiscard]] std::chrono::milliseconds elapsed() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_);
  }

  [[nodiscard]] std::chrono::steady_clock::time_point started() const noexcept {
    return started_;
  }

private:
  std::chrono::steady_clock::time_point started_;
  std::chrono::steady_clock::time_point deadline_;
};

[[nodiscard]] CommandFailure cancelled_failure() {
  return CommandFailure{.kind = FailureKind::cancelled,
                        .delivery = DeliveryStatus::not_started,
                        .exit_code = 0,
                        .diagnostic = "the wait was cancelled"};
}

[[nodiscard]] bool gave_up(const WaitOptions& options) {
  return options.cancelled && options.cancelled();
}

// The whole wait in one place, so that the several paths below cannot drift
// on what they carry.
struct Wait {
  const Server& server;
  const std::string& pane_id;
  std::string_view wanted;
  const WaitOptions& options;
  Deadline deadline;
  std::size_t match_budget;
  // Asked of the caller once, when the pane is known, rather than on every
  // capture: the answer is the same all the way through and the caller may be
  // taking a lock to produce it.
  std::vector<std::string> sent{};
  // Empty until looked up, and only looked up when a connection is about to
  // open. The `Server` entry point already has it and fills it in.
  std::string session_name{};
  bool session_known{};
  bool matched_at_entry{};
};

// Absent means the budget ran out rather than the command failing: a wait
// whose deadline passes mid-capture has timed out, which is an outcome, not an
// error.
using BoundedOutput = expected<std::optional<std::string>, CommandFailure>;

[[nodiscard]] BoundedOutput run_before_deadline(const Wait& wait,
                                                std::vector<std::string> command) {
  if (gave_up(wait.options)) {
    return unexpected(cancelled_failure());
  }
  const auto remaining = wait.deadline.remaining();
  if (!remaining.has_value()) {
    return std::optional<std::string>{};
  }
  auto reply = wait.server.run(command, *remaining);
  if (!reply.has_value()) {
    if (reply.error().kind == FailureKind::timeout) {
      return std::optional<std::string>{};
    }
    return unexpected(std::move(reply.error()));
  }
  if (gave_up(wait.options)) {
    return unexpected(cancelled_failure());
  }
  return std::optional<std::string>{*std::move(reply)};
}

// `-J`: without it tmux reports a soft-wrapped line — one logical line that
// ran past the pane's width — as separate lines split at the wrap column, with
// no marker that the break is not a real one. Text straddling that column then
// never matches, and `output_confirms` also reads the wrong row as the active
// one. Not `Pane::capture`, which cannot be given the remaining budget: a
// capture that outlives the deadline would turn a timeout into a hang.
[[nodiscard]] BoundedOutput capture_before_deadline(const Wait& wait) {
  return run_before_deadline(wait, {"capture-pane", "-p", "-J", "-t", wait.pane_id});
}

// Charge a search against the budget before running it. A pane printing faster
// than this can read it would otherwise spin until the deadline with nothing
// to show.
[[nodiscard]] expected<bool, CommandFailure> within_budget(Wait& wait,
                                                           std::string_view text) {
  if (text.size() > wait.match_budget ||
      wait.wanted.size() > wait.match_budget - text.size()) {
    return unexpected(
        CommandFailure{.kind = FailureKind::truncated,
                       .delivery = DeliveryStatus::replied,
                       .exit_code = 0,
                       .diagnostic = "wait matching work limit exceeded"});
  }
  wait.match_budget -= text.size() + wait.wanted.size();
  return text.find(wait.wanted) != std::string_view::npos;
}

[[nodiscard]] WaitResult matched_result(const Wait& wait, WaitPath path,
                                        std::string text) {
  return WaitResult{.matched = true,
                    .matched_at_entry = wait.matched_at_entry,
                    .elapsed = wait.deadline.elapsed(),
                    .path = path,
                    .pane_id = wait.pane_id,
                    .text = std::move(text)};
}

// Every path defers a match it cannot credit to the pane, treating it exactly
// as "not yet" and capturing again. This is where every timeout is built, and
// a deferred match still standing at the deadline is never promoted: text the
// caller itself sent is not output, however new the bytes are. It is reported
// separately instead, so a caller can tell "I saw it but could not credit it"
// apart from a plain silent timeout.
[[nodiscard]] WaitResult timed_out(const Wait& wait, WaitPath path, std::string text) {
  if (!wait.wanted.empty() && output_confirms(text, wait.wanted, wait.sent)) {
    return matched_result(wait, path, std::move(text));
  }
  const bool present =
      !wait.wanted.empty() && text.find(wait.wanted) != std::string::npos;
  return WaitResult{.matched_at_entry = wait.matched_at_entry,
                    .timed_out = true,
                    .present_unconfirmed = present,
                    .elapsed = wait.deadline.elapsed(),
                    .path = path,
                    .pane_id = wait.pane_id,
                    .text = std::move(text)};
}

void report(const Wait& wait, WaitPath path) {
  if (wait.options.on_progress) {
    wait.options.on_progress(wait.deadline.elapsed(), path);
  }
}

[[nodiscard]] expected<WaitResult, CommandFailure>
poll_for_text(Wait& wait, WaitPath path, std::string last) {
  auto next_progress = wait.deadline.started();
  while (!wait.deadline.expired()) {
    if (gave_up(wait.options)) {
      return unexpected(cancelled_failure());
    }
    auto captured = capture_before_deadline(wait);
    if (!captured.has_value()) {
      return unexpected(std::move(captured.error()));
    }
    if (!captured->has_value()) {
      return timed_out(wait, path, std::move(last));
    }
    last = *std::move(*captured);
    if (wait.deadline.expired()) {
      return timed_out(wait, path, std::move(last));
    }
    auto present = within_budget(wait, last);
    if (!present.has_value()) {
      return unexpected(std::move(present.error()));
    }
    if (*present && output_confirms(last, wait.wanted, wait.sent)) {
      return matched_result(wait, path, std::move(last));
    }
    if (std::chrono::steady_clock::now() >= next_progress) {
      report(wait, path);
      next_progress = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    }
    const auto remaining = wait.deadline.remaining();
    if (remaining.has_value()) {
      std::this_thread::sleep_for(std::min(*remaining, wait.options.poll_interval));
    }
  }
  return timed_out(wait, path, std::move(last));
}

[[nodiscard]] expected<WaitResult, CommandFailure>
stream_for_text(Wait& wait, std::string_view session_name, std::string screen) {
  if (gave_up(wait.options)) {
    return unexpected(cancelled_failure());
  }
  // Not `#{socket_path}`: tmux escapes non-printable bytes in the socket path
  // when it stores it at server start, so a socket named with one comes back
  // as its `\376` spelling and names no file. Connecting to that fails, and
  // this would fall back to polling without ever saying why. The handle
  // already knows the exact bytes every command here travels over.
  const std::string_view socket = wait.server.socket_path();
  if (socket.empty() || session_name.empty()) {
    return poll_for_text(wait, WaitPath::capture_polling, std::move(screen));
  }
  const auto remaining = wait.deadline.remaining();
  if (!remaining.has_value()) {
    return timed_out(wait, WaitPath::capture_before_control, std::move(screen));
  }

  ConnectionOptions options;
  options.socket_path = std::string{socket};
  options.session_name = std::string{session_name};
  options.startup_timeout = std::min(*remaining, std::chrono::milliseconds{2000});
  options.shutdown_timeout = std::min(*remaining, std::chrono::milliseconds{500});
  options.pane_output = true;
  options.pause_after = std::chrono::seconds{2};
  auto connected = Connection::connect(std::move(options));
  if (!connected.has_value()) {
    return poll_for_text(wait, WaitPath::capture_polling, std::move(screen));
  }
  Connection connection = *std::move(connected);
  // Held for exactly as long as the connection is open, so a caller counting
  // attached clients does not count this wait's own observation as somebody
  // else.
  std::shared_ptr<void> observed;
  if (wait.options.observe_control_client) {
    observed = wait.options.observe_control_client(connection.native_child_pid());
  }
  if (gave_up(wait.options)) {
    return unexpected(cancelled_failure());
  }

  // The window between the entry capture and the first notification belongs to
  // nobody otherwise: output that landed while the connection was starting
  // would not appear on the wire and would not have been on screen yet.
  auto after_connect = capture_before_deadline(wait);
  if (!after_connect.has_value()) {
    return unexpected(std::move(after_connect.error()));
  }
  if (!after_connect->has_value()) {
    return timed_out(wait, WaitPath::capture_after_control_connect, std::move(screen));
  }
  screen = *std::move(*after_connect);
  if (wait.deadline.expired()) {
    return timed_out(wait, WaitPath::capture_after_control_connect, std::move(screen));
  }
  auto present = within_budget(wait, screen);
  if (!present.has_value()) {
    return unexpected(std::move(present.error()));
  }
  if (*present && output_confirms(screen, wait.wanted, wait.sent)) {
    return matched_result(wait, WaitPath::capture_after_control_connect,
                          std::move(screen));
  }

  auto next_progress = wait.deadline.started();
  while (!wait.deadline.expired()) {
    if (gave_up(wait.options)) {
      return unexpected(cancelled_failure());
    }
    const auto slice = wait.deadline.remaining();
    if (!slice.has_value()) {
      break;
    }
    const auto slice_deadline =
        std::chrono::steady_clock::now() + std::min(*slice, wait.options.poll_interval);
    auto notifications = connection.wait_for_notifications(slice_deadline);
    // Back early with nothing is the connection saying it has stopped
    // delivering, not a quiet pane: a quiet pane uses the whole slice. Falling
    // back to capture keeps the wait answering rather than blocking on a wire
    // that will stay silent.
    if (notifications.empty() &&
        std::chrono::steady_clock::now() + std::chrono::milliseconds{5} <
            slice_deadline) {
      return poll_for_text(wait, WaitPath::capture_polling, std::move(screen));
    }
    for (const Notification& notification : notifications) {
      const ParsedNotification parsed = parse(notification);
      if ((parsed.kind != NotificationKind::output &&
           parsed.kind != NotificationKind::extended_output) ||
          parsed.pane != wait.pane_id) {
        continue;
      }
      // The notification says the pane produced something, not what the screen
      // now reads: `%output` carries the bytes written, which a redraw, a
      // wrap or a clear can place anywhere. The capture is what gets searched.
      auto captured = capture_before_deadline(wait);
      if (!captured.has_value()) {
        return unexpected(std::move(captured.error()));
      }
      if (!captured->has_value()) {
        return timed_out(wait, WaitPath::control_output, std::move(screen));
      }
      screen = *std::move(*captured);
      if (wait.deadline.expired()) {
        return timed_out(wait, WaitPath::control_output, std::move(screen));
      }
      auto found = within_budget(wait, screen);
      if (!found.has_value()) {
        return unexpected(std::move(found.error()));
      }
      if (*found && output_confirms(screen, wait.wanted, wait.sent)) {
        return matched_result(wait, WaitPath::control_output, std::move(screen));
      }
    }
    if (std::chrono::steady_clock::now() >= next_progress) {
      report(wait, WaitPath::control_output);
      next_progress = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    }
  }
  return timed_out(wait, WaitPath::control_output, std::move(screen));
}

// Both entry points meet here, with the deadline already running and the pane
// settled: a wait that had to resolve its target first has already spent part
// of its budget doing so, and must not start over.
[[nodiscard]] expected<WaitResult, CommandFailure> perform(Wait& wait) {
  if (wait.options.sent) {
    wait.sent = wait.options.sent(wait.pane_id);
  }
  auto captured = capture_before_deadline(wait);
  if (!captured.has_value()) {
    return unexpected(std::move(captured.error()));
  }
  if (!captured->has_value()) {
    return timed_out(wait, WaitPath::capture_at_entry, {});
  }
  std::string screen = *std::move(*captured);
  // The screen at entry is read before anything else runs, so a later capture
  // that merely rediscovers text that was already here is never reported as
  // fresh output.
  auto present = within_budget(wait, screen);
  if (!present.has_value()) {
    return unexpected(std::move(present.error()));
  }
  wait.matched_at_entry = *present;
  if (wait.deadline.expired()) {
    return timed_out(wait, WaitPath::capture_at_entry, std::move(screen));
  }
  if (*present && output_confirms(screen, wait.wanted, wait.sent)) {
    return matched_result(wait, WaitPath::capture_at_entry, std::move(screen));
  }

  // Only now, and only for the connection: `-t =name` matches a session by
  // exact name, so a pane or session id will not do, and a wait the entry
  // capture answers should not pay for a lookup it never needed. The `Server`
  // entry point learned the name with the pane and has already filled it in.
  if (!wait.session_known) {
    if (auto owning =
            run_before_deadline(wait, {"display-message", "-p", "-t", wait.pane_id,
                                       "--", "#{session_name}"});
        owning.has_value() && owning->has_value()) {
      wait.session_name = *std::move(*owning);
      while (!wait.session_name.empty() &&
             (wait.session_name.back() == '\n' || wait.session_name.back() == '\r')) {
        wait.session_name.pop_back();
      }
    }
    wait.session_known = true;
  }
  return stream_for_text(wait, wait.session_name, std::move(screen));
}

} // namespace

expected<WaitResult, CommandFailure>
Pane::wait_for_text(std::string_view wanted, const WaitOptions& options) const {
  if (auto refusal = refused(ServerFeature::pane_io,
                             "psmux can capture the active pane for a stale target")) {
    return unexpected(std::move(*refusal));
  }
  auto server = this->server();
  if (!server.has_value()) {
    return unexpected(std::move(server.error()));
  }
  const std::string pane_id{id().value()};
  Wait wait{.server = *server,
            .pane_id = pane_id,
            .wanted = wanted,
            .options = options,
            .deadline = Deadline{options.timeout},
            .match_budget = options.match_budget};
  return perform(wait);
}

expected<WaitResult, CommandFailure>
Server::wait_for_text(std::string_view target, std::string_view wanted,
                      const WaitOptions& options) const {
  // One deadline covers the lookup and the wait, because the caller's budget is
  // for the whole question: a target that will not resolve must not be able to
  // spend it all and leave nothing for waiting.
  Deadline deadline{options.timeout};
  std::string pane_id;
  std::string session_name;
  {
    // Both fields in one command. The pane is what gets captured; the session
    // is what the control connection attaches to, and asking separately would
    // pay a second round trip for something tmux will answer at the same time.
    constexpr std::array fields{std::string_view{"pane_id"},
                                std::string_view{"session_name"}};
    Wait lookup{.server = *this,
                .pane_id = pane_id,
                .wanted = wanted,
                .options = options,
                .deadline = deadline,
                .match_budget = options.match_budget};
    auto reply =
        run_before_deadline(lookup, {"display-message", "-p", "-t", std::string{target},
                                     "--", std::string{format_request(fields)}});
    if (!reply.has_value()) {
      return unexpected(std::move(reply.error()));
    }
    if (!reply->has_value()) {
      return WaitResult{.timed_out = true,
                        .elapsed = deadline.elapsed(),
                        .path = WaitPath::pane_lookup};
    }
    auto snapshot = Snapshot::from_recording(fields, *std::move(*reply));
    if (snapshot == nullptr || snapshot->rows().size() != 1U ||
        snapshot->rows().front()[0].empty()) {
      return unexpected(CommandFailure{
          .kind = FailureKind::missing,
          .delivery = DeliveryStatus::replied,
          .exit_code = 0,
          .diagnostic = "tmux could not resolve the pane " + std::string{target}});
    }
    pane_id = std::string{snapshot->rows().front()[0]};
    session_name = std::string{snapshot->rows().front()[1]};
  }
  Wait wait{.server = *this,
            .pane_id = pane_id,
            .wanted = wanted,
            .options = options,
            .deadline = deadline,
            .match_budget = options.match_budget,
            .session_name = session_name,
            .session_known = true};
  return perform(wait);
}

LIBTMUX_NAMESPACE_END
