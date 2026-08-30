#include "process.hpp"

#include "libtmux/expected.hpp"
#include "posix_child.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <limits>
#include <optional>
#include <utility>

#include <poll.h>

LIBTMUX_NAMESPACE_BEGIN
namespace detail {
namespace {

using Clock = ChildClock;

constexpr auto poll_quantum = std::chrono::milliseconds{10};
constexpr auto terminate_grace = std::chrono::milliseconds{100};
constexpr auto post_exit_drain = std::chrono::milliseconds{100};
constexpr auto kill_reap_grace = std::chrono::milliseconds{500};

[[nodiscard]] int poll_timeout(Clock::time_point boundary) {
  const auto now = Clock::now();
  if (now >= boundary) {
    return 0;
  }
  const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(boundary - now);
  // Explicit, because milliseconds::rep is long on one standard library and
  // long long on another, and deduction picks neither.
  const auto bounded = std::min<std::chrono::milliseconds::rep>(
      remaining.count(), std::numeric_limits<int>::max());
  return static_cast<int>(bounded);
}

// Waits for either stream, then reads whichever are ready. Everything it can
// fail at belongs to the child; only the waiting is this runner's.
[[nodiscard]] std::optional<ProcessError>
poll_and_drain(PosixChild& child, Clock::time_point boundary, DeliveryStatus delivery) {
  std::array<pollfd, 2> descriptors{{
      {.fd = child.descriptor(ChildStream::stdout_stream),
       .events = POLLIN,
       .revents = 0},
      {.fd = child.descriptor(ChildStream::stderr_stream),
       .events = POLLIN,
       .revents = 0},
  }};
  for (;;) {
    if (Clock::now() >= boundary) {
      return std::nullopt;
    }
    if (::poll(descriptors.data(), descriptors.size(), poll_timeout(boundary)) >= 0) {
      break;
    }
    if (errno != EINTR) {
      return process_error(ProcessError::Kind::pipe, delivery, "pipe poll",
                           child.rendered_request(),
                           std::error_code{errno, std::generic_category()});
    }
  }

  const auto ready = [&](pollfd descriptor,
                         ChildStream stream) -> std::optional<ProcessError> {
    if (child.descriptor(stream) < 0) {
      return std::nullopt;
    }
    if ((descriptor.revents & POLLNVAL) != 0) {
      child.close_stream(stream);
      return process_error(ProcessError::Kind::pipe, delivery, "pipe poll",
                           child.rendered_request(),
                           std::error_code{EBADF, std::generic_category()});
    }
    if ((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      return child.drain(stream, boundary, delivery);
    }
    return std::nullopt;
  };

  if (auto failure = ready(descriptors[0], ChildStream::stdout_stream)) {
    return failure;
  }
  return ready(descriptors[1], ChildStream::stderr_stream);
}

// Ends the child and reads what it left. Every caller is already returning a
// failure, so nothing here reports one of its own.
void cleanup_group(PosixChild& child, bool allow_term) {
  const auto settle = [&](Clock::time_point until) {
    while (child.status() == ChildStatus::running && Clock::now() < until) {
      if (child.update_status(DeliveryStatus::indeterminate).has_value()) {
        break;
      }
      if (poll_and_drain(child, std::min(until, Clock::now() + poll_quantum),
                         DeliveryStatus::indeterminate)) {
        // Reading failed and this path is already returning an error, so
        // stop reading and go on waiting for the child. Polling two closed
        // descriptors is what paces the wait from here.
        child.close_output();
      }
    }
  };

  if (allow_term) {
    child.signal_group(SIGTERM);
    settle(Clock::now() + terminate_grace);
  }
  child.signal_group(SIGKILL);
  settle(Clock::now() + kill_reap_grace);
  child.wait_for_exit();

  const auto drain_deadline = Clock::now() + post_exit_drain;
  while (!child.output_closed() && Clock::now() < drain_deadline) {
    if (poll_and_drain(child, std::min(drain_deadline, Clock::now() + poll_quantum),
                       DeliveryStatus::indeterminate)) {
      break;
    }
  }
  child.close_output();
}

// The capture is attached by the caller, after the cleanup that keeps
// reading.
[[nodiscard]] ProcessError timed_out(PosixChild& child, DeliveryStatus delivery) {
  return process_error(ProcessError::Kind::timeout, delivery, "timeout",
                       child.rendered_request(),
                       std::make_error_code(std::errc::timed_out));
}

} // namespace

expected<ProcessReply, ProcessError> run_process(const ProcessRequest& request) {
  if (request.timeout.has_value() &&
      *request.timeout <= std::chrono::milliseconds::zero()) {
    return unexpected(process_error(
        ProcessError::Kind::timeout, DeliveryStatus::not_started, "timeout",
        render_request(request), std::make_error_code(std::errc::timed_out)));
  }
  std::optional<Clock::time_point> deadline;
  if (request.timeout.has_value()) {
    deadline = Clock::now() + *request.timeout;
  }

  auto launched = PosixChild::launch(request);
  if (!launched.has_value()) {
    return unexpected(std::move(launched.error()));
  }
  auto& child = *launched;

  const auto abandon = [&](ProcessError error, bool allow_term) {
    cleanup_group(child, allow_term);
    auto capture = child.take_capture();
    error.stdout_bytes = std::move(capture.stdout_bytes);
    error.stderr_bytes = std::move(capture.stderr_bytes);
    error.output_truncated = capture.truncated;
    return unexpected(std::move(error));
  };

  if (deadline.has_value() && Clock::now() >= *deadline) {
    return abandon(timed_out(child, DeliveryStatus::indeterminate), true);
  }
  if (const auto teardown = child.spawn_teardown_error(); teardown != 0) {
    return abandon(process_error(ProcessError::Kind::pipe,
                                 DeliveryStatus::indeterminate, "pipe",
                                 child.rendered_request(),
                                 std::error_code{teardown, std::generic_category()}),
                   false);
  }

  auto exit_drain_deadline = Clock::time_point::max();
  for (;;) {
    if (auto failure = child.update_status(DeliveryStatus::indeterminate)) {
      return abandon(std::move(*failure), false);
    }
    const bool finished = child.status() != ChildStatus::running;
    if (finished && exit_drain_deadline == Clock::time_point::max()) {
      exit_drain_deadline = Clock::now() + post_exit_drain;
    }
    if (finished && child.output_closed()) {
      break;
    }
    if (finished && Clock::now() >= exit_drain_deadline) {
      child.close_output();
      break;
    }
    if (!finished && deadline.has_value() && Clock::now() >= *deadline) {
      return abandon(timed_out(child, DeliveryStatus::indeterminate), true);
    }

    const auto poll_boundary = Clock::now() + poll_quantum;
    const auto boundary =
        finished ? std::min(exit_drain_deadline, poll_boundary)
                 : (deadline.has_value() ? std::min(*deadline, poll_boundary)
                                         : poll_boundary);
    if (auto failure = poll_and_drain(child, boundary, DeliveryStatus::indeterminate)) {
      return abandon(std::move(*failure), false);
    }
  }

  auto capture = child.take_capture();
  return ProcessReply{
      .termination = child.termination(),
      .stdout_bytes = std::move(capture.stdout_bytes),
      .stderr_bytes = std::move(capture.stderr_bytes),
      .output_truncated = capture.truncated,
  };
}

} // namespace detail
LIBTMUX_NAMESPACE_END
