#pragma once

// One deadline for one call.
//
// A call is a version check, a dispatch and the wait for the answer. Each of
// them used to take the caller's timeout as if it were its own, so a 200 ms call
// could spend 200 ms asking which tmux this was and another 200 ms running the
// command. A `Deadline` is made once, where the call enters the library, and
// every phase after that asks it how much is left instead of being handed the
// original number.

#include "libtmux/abi.hpp"
#include "libtmux/command.hpp"
#include <chrono>
#include <optional>
#include <string_view>

LIBTMUX_NAMESPACE_BEGIN
namespace detail {

class Deadline {
public:
  /// A call that waits as long as it takes: `wait_for` asks for this.
  [[nodiscard]] static Deadline unbounded() noexcept { return Deadline{}; }

  /// A call that must be over `budget` from now. An absent budget is unbounded,
  /// and so is one past the clock's range; a negative one is already over.
  [[nodiscard]] static Deadline
  within(std::optional<std::chrono::milliseconds> budget) noexcept {
    using Clock = std::chrono::steady_clock;
    Deadline deadline;
    if (!budget.has_value()) {
      return deadline;
    }
    const auto now = Clock::now();
    if (*budget <= std::chrono::milliseconds::zero()) {
      deadline.end_ = now;
    } else if (*budget >= std::chrono::duration_cast<std::chrono::milliseconds>(
                              Clock::time_point::max() - now)) {
      // Past the clock's range: adding it would overflow, and no call lasts
      // that long. Treated as no deadline.
    } else {
      deadline.end_ = now + *budget;
    }
    return deadline;
  }

  [[nodiscard]] bool bounded() const noexcept { return end_.has_value(); }

  [[nodiscard]] bool expired() const noexcept {
    return end_.has_value() && std::chrono::steady_clock::now() >= *end_;
  }

  /// What is left, rounded up so a deadline that has not passed never reads as
  /// zero. Absent when there is no deadline. Zero once it has passed.
  [[nodiscard]] std::optional<std::chrono::milliseconds> remaining() const noexcept {
    if (!end_.has_value()) {
      return std::nullopt;
    }
    const auto left = *end_ - std::chrono::steady_clock::now();
    if (left <= std::chrono::steady_clock::duration::zero()) {
      return std::chrono::milliseconds{0};
    }
    return std::chrono::ceil<std::chrono::milliseconds>(left);
  }

  /// The failure for a phase that found nothing left. It is a timeout that
  /// stopped before the command was written, so it is safe to retry.
  [[nodiscard]] static CommandFailure exhausted(std::string_view phase) {
    return CommandFailure{.kind = FailureKind::timeout,
                          .delivery = DeliveryStatus::not_started,
                          .exit_code = 0,
                          .diagnostic = std::string{phase} +
                                        " used the whole deadline of this call"};
  }

private:
  std::optional<std::chrono::steady_clock::time_point> end_;
};

} // namespace detail
LIBTMUX_NAMESPACE_END
