#pragma once

// One POSIX child: validation, launch, bounded capture, status and signals.
//
// The blocking runner and the asynchronous engine differ in how they wait for
// a child, not in what owning one means, so the mechanics live here once. An
// owner that was forked instead would have to be corrected twice.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sys/types.h>

#include "process.hpp"

LIBTMUX_NAMESPACE_BEGIN
namespace detail {

using ChildClock = std::chrono::steady_clock;

enum class ChildStream : std::uint8_t { stdout_stream, stderr_stream };

// A reaped child and a child whose status can never be read are different
// facts. Holding both in one bool let a lost status read as a clean exit.
enum class ChildStatus : std::uint8_t { running, exited, unknowable, none };

// The polling value exercises the same path platforms without pidfds use.
enum class ExitReadiness : std::uint8_t { platform, poll };

struct Capture final {
  std::vector<std::byte> stdout_bytes;
  std::vector<std::byte> stderr_bytes;
  bool truncated{false};
};

// The request as it may be shown: a secret argument is replaced rather than
// escaped, and every other byte outside printable ASCII is hex-escaped.
[[nodiscard]] std::string render_request(const ProcessRequest& request);

[[nodiscard]] ProcessError process_error(ProcessError::Kind kind,
                                         DeliveryStatus delivery,
                                         std::string_view operation,
                                         std::string_view rendered_request,
                                         std::error_code cause, Capture capture = {});

class PosixChild final {
public:
  // Validates, opens the pipes the policy asks for, and spawns. A returned
  // child owns its process group and its descriptors.
  [[nodiscard]] static expected<PosixChild, ProcessError>
  launch(const ProcessRequest& request,
         ExitReadiness exit_readiness = ExitReadiness::platform);

  ~PosixChild() noexcept;
  PosixChild(PosixChild&& other) noexcept;
  PosixChild& operator=(PosixChild&& other) noexcept;
  PosixChild(const PosixChild&) = delete;
  PosixChild& operator=(const PosixChild&) = delete;

  [[nodiscard]] pid_t pid() const noexcept;
  [[nodiscard]] int descriptor(ChildStream stream) const noexcept;
  // Readable exactly when the child has exited, where the platform can say so.
  // Negative elsewhere, and a caller then has to ask on its own turn instead.
  [[nodiscard]] int exit_descriptor() const noexcept;
  [[nodiscard]] ChildStatus status() const noexcept;
  [[nodiscard]] bool output_closed() const noexcept;
  [[nodiscard]] std::string_view rendered_request() const noexcept;
  // Non-zero when releasing the spawn attributes failed. The child is
  // running regardless, so its owner reports this rather than the launch.
  [[nodiscard]] int spawn_teardown_error() const noexcept;

  // Reads until the stream would block, ends, or the boundary passes. The
  // boundary is what stops a child that never stops writing.
  [[nodiscard]] std::optional<ProcessError> drain(ChildStream stream,
                                                  ChildClock::time_point boundary,
                                                  DeliveryStatus delivery) noexcept;
  // One chunk leaves the rest of a reactor turn to the other children.
  [[nodiscard]] std::optional<ProcessError>
  drain_once(ChildStream stream, ChildClock::time_point boundary,
             DeliveryStatus delivery) noexcept;
  [[nodiscard]] std::optional<ProcessError>
  update_status(DeliveryStatus delivery) noexcept;
  void signal_group(int signal_number) noexcept;
  // Blocks until the child is collected. Only for a caller that has already
  // killed it.
  void wait_for_exit() noexcept;
  // Closing a stream before it ends means the rest was never read.
  void close_stream(ChildStream stream) noexcept;
  void close_output() noexcept;

  [[nodiscard]] Capture take_capture() noexcept;
  [[nodiscard]] Termination termination() const noexcept;

private:
  PosixChild(pid_t pid, int stdout_fd, int stderr_fd, int exit_fd,
             std::size_t capture_limit, std::string rendered) noexcept;
  void close_descriptor(ChildStream stream) noexcept;
  [[nodiscard]] std::optional<ProcessError> drain_impl(ChildStream stream,
                                                       ChildClock::time_point boundary,
                                                       DeliveryStatus delivery,
                                                       bool one_read) noexcept;
  // Closed as soon as the status is known, so a caller woken by the
  // answer is not still holding a descriptor for a finished child.
  void close_exit_descriptor() noexcept;

  pid_t pid_{-1};
  int stdout_fd_{-1};
  int stderr_fd_{-1};
  int exit_fd_{-1};
  std::size_t capture_limit_{0U};
  std::string rendered_request_;
  Capture capture_;
  int wait_status_{0};
  ChildStatus status_{ChildStatus::none};
  int spawn_teardown_error_{0};
};

} // namespace detail
LIBTMUX_NAMESPACE_END
