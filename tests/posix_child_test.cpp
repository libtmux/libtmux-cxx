// The POSIX child owner, against real processes. Ownership is only proven by
// actual pipe, signal and reap facts, so nothing here is scripted.
#include "posix_child.hpp"
#include "spawn_descriptors.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <mutex>
#include <string>
#include <thread>
#include <variant>

#include <fcntl.h>
#include <poll.h>
#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#endif
#include <pthread.h>

namespace {

using libtmux::detail::ChildClock;
using libtmux::detail::ChildStatus;
using libtmux::detail::ChildStream;
using libtmux::detail::Exited;
using libtmux::detail::ExitReadiness;
using libtmux::detail::PosixChild;
using libtmux::detail::ProcessError;
using libtmux::detail::ProcessRequest;
using libtmux::detail::Signaled;

#if defined(__linux__)
struct LateMarker final {
  std::mutex mutex;
  std::condition_variable changed;
  std::array<int, 2> descriptors{-1, -1};
  int error{0};
  int policy_error{0};
  bool policy_ready{false};
  bool marker_ready{false};
  bool cancelled{false};

  void open_after_policy() {
    std::unique_lock lock{mutex};
    changed.wait(lock, [this] { return policy_ready || cancelled; });
    if (cancelled || policy_error != 0) {
      return;
    }
    lock.unlock();
    if (::pipe(descriptors.data()) != 0) {
      error = errno;
    }
    lock.lock();
    marker_ready = true;
    changed.notify_all();
  }

  void finish_policy(int result) {
    std::unique_lock lock{mutex};
    policy_error = result;
    policy_ready = true;
    changed.notify_all();
    if (result == 0) {
      changed.wait(lock, [this] { return marker_ready || cancelled; });
    }
  }

  void cancel() {
    std::lock_guard lock{mutex};
    cancelled = true;
    changed.notify_all();
  }
};

LateMarker* pending_marker = nullptr;

void finish_descriptor_policy(int result) { pending_marker->finish_policy(result); }
#endif

ProcessRequest shell(std::string script) {
  ProcessRequest request;
  request.executable = "/bin/sh";
  request.arguments = {{"-c"}, {std::move(script)}};
  return request;
}

std::string text(const std::vector<std::byte>& value) {
  std::string result;
  for (const auto byte : value) {
    result.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
  }
  return result;
}

// Drains both pipes and collects the status, exactly as a reactor would:
// readiness first, and the child's status is a separate fact from its output
// ending.
void run_to_completion(PosixChild& child) {
  const auto give_up = ChildClock::now() + std::chrono::seconds{10};
  while ((!child.output_closed() || child.status() == ChildStatus::running) &&
         ChildClock::now() < give_up) {
    std::array<pollfd, 2> watched{
        pollfd{.fd = child.descriptor(ChildStream::stdout_stream),
               .events = POLLIN,
               .revents = 0},
        pollfd{.fd = child.descriptor(ChildStream::stderr_stream),
               .events = POLLIN,
               .revents = 0}};
    static_cast<void>(::poll(watched.data(), watched.size(), 20));
    const auto boundary = ChildClock::now() + std::chrono::milliseconds{50};
    static_cast<void>(child.drain(ChildStream::stdout_stream, boundary,
                                  libtmux::DeliveryStatus::indeterminate));
    static_cast<void>(child.drain(ChildStream::stderr_stream, boundary,
                                  libtmux::DeliveryStatus::indeterminate));
    static_cast<void>(child.update_status(libtmux::DeliveryStatus::indeterminate));
  }
}

TEST(PosixChild, CapturesBothStreamsAndReapsTheDirectChild) {
  auto launched = PosixChild::launch(shell("printf out; printf err >&2"));
  ASSERT_TRUE(launched.has_value()) << launched.error().diagnostic;
  run_to_completion(*launched);

  ASSERT_EQ(launched->status(), ChildStatus::exited);
  ASSERT_TRUE(std::holds_alternative<Exited>(launched->termination()));
  EXPECT_EQ(std::get<Exited>(launched->termination()).code, 0);
  auto capture = launched->take_capture();
  EXPECT_EQ(text(capture.stdout_bytes), "out");
  EXPECT_EQ(text(capture.stderr_bytes), "err");
  EXPECT_FALSE(capture.truncated);
}

TEST(PosixChild, KeepsDrainingPastItsCaptureLimit) {
  auto request = shell("i=0; while [ $i -lt 400 ]; do printf '0123456789'; "
                       "i=$((i+1)); done");
  request.capture_limit = 64U;
  auto launched = PosixChild::launch(std::move(request));
  ASSERT_TRUE(launched.has_value()) << launched.error().diagnostic;
  run_to_completion(*launched);

  ASSERT_EQ(launched->status(), ChildStatus::exited);
  // The child ran to its own end rather than dying on a closed pipe.
  ASSERT_TRUE(std::holds_alternative<Exited>(launched->termination()));
  EXPECT_EQ(std::get<Exited>(launched->termination()).code, 0);
  auto capture = launched->take_capture();
  EXPECT_EQ(capture.stdout_bytes.size(), 64U);
  EXPECT_TRUE(capture.truncated);
}

// The write end becomes the child's stdout. A child that finds it non-blocking
// gets EAGAIN on a burst larger than the pipe buffer, which it reports as a
// write error rather than waiting for the reader.
TEST(PosixChild, DoesNotHandTheChildANonBlockingStdout) {
  auto request = shell("seq 1 40000");
  request.capture_limit = 1024U * 1024U;
  auto launched = PosixChild::launch(std::move(request));
  ASSERT_TRUE(launched.has_value()) << launched.error().diagnostic;
  // Let the pipe fill first, which is what any delayed reader does.
  std::this_thread::sleep_for(std::chrono::milliseconds{150});
  run_to_completion(*launched);

  ASSERT_EQ(launched->status(), ChildStatus::exited);
  ASSERT_TRUE(std::holds_alternative<Exited>(launched->termination()));
  EXPECT_EQ(std::get<Exited>(launched->termination()).code, 0);
  EXPECT_GT(launched->take_capture().stdout_bytes.size(), 200000U);
}

TEST(PosixChild, SignalsTheWholeGroupAndKeepsTheSignalStatus) {
  auto launched = PosixChild::launch(shell("sleep 60 & wait"));
  ASSERT_TRUE(launched.has_value()) << launched.error().diagnostic;
  launched->signal_group(SIGKILL);
  run_to_completion(*launched);

  ASSERT_EQ(launched->status(), ChildStatus::exited);
  ASSERT_TRUE(std::holds_alternative<Signaled>(launched->termination()));
  EXPECT_EQ(std::get<Signaled>(launched->termination()).signal, SIGKILL);
}

// Where the platform can make exit a readable descriptor, the child must
// actually have one: a reactor that silently fell back to asking would still
// pass every test that only checks the answer.
TEST(PosixChild, WaitsOnExitWherePlatformAllows) {
  auto launched = PosixChild::launch(shell("exit 0"));
  ASSERT_TRUE(launched.has_value()) << launched.error().diagnostic;
#if defined(__linux__) && defined(SYS_pidfd_open) &&                                   \
    !defined(LIBTMUX_FORCE_PORTABLE_SYSCALLS)
  EXPECT_GE(launched->exit_descriptor(), 0);
#else
  EXPECT_LT(launched->exit_descriptor(), 0);
#endif
  run_to_completion(*launched);
  EXPECT_EQ(launched->status(), ChildStatus::exited);
}

TEST(PosixChild, APortableDrainTurnReadsAReadyChunkPastItsBoundary) {
  auto launched =
      PosixChild::launch(shell("printf prefix; sleep 30"), ExitReadiness::poll);
  ASSERT_TRUE(launched.has_value()) << launched.error().diagnostic;
  ASSERT_LT(launched->exit_descriptor(), 0);
  pollfd watched{.fd = launched->descriptor(ChildStream::stdout_stream),
                 .events = POLLIN,
                 .revents = 0};
  ASSERT_EQ(::poll(&watched, 1U, 1000), 1);
  ASSERT_NE(watched.revents & POLLIN, 0);

  static_cast<void>(launched->drain_once(
      ChildStream::stdout_stream, ChildClock::now() - std::chrono::milliseconds{1},
      libtmux::DeliveryStatus::indeterminate));
  auto capture = launched->take_capture();

  EXPECT_EQ(text(capture.stdout_bytes), "prefix");
  launched->signal_group(SIGKILL);
  run_to_completion(*launched);
}

TEST(PosixChild, ReportsAMissingExecutableWithoutStarting) {
  ProcessRequest request;
  request.executable = "/nonexistent/tmux";
  auto launched = PosixChild::launch(request);
  ASSERT_FALSE(launched.has_value());
  EXPECT_EQ(launched.error().kind, ProcessError::Kind::spawn);
  EXPECT_EQ(launched.error().delivery, libtmux::DeliveryStatus::not_started);
}

// A process that ignores SIGCHLD has told the kernel to reap for it, so
// waitpid answers ECHILD and the status is gone rather than zero.
TEST(PosixChild, DoesNotReportALostStatusAsACleanExit) {
  struct sigaction ignored {};
  ignored.sa_handler = SIG_IGN;
  sigemptyset(&ignored.sa_mask);
  struct sigaction previous {};
  ASSERT_EQ(::sigaction(SIGCHLD, &ignored, &previous), 0);
  auto launched = PosixChild::launch(shell("exit 3"));
  ASSERT_TRUE(launched.has_value()) << launched.error().diagnostic;
  run_to_completion(*launched);
  ASSERT_EQ(::sigaction(SIGCHLD, &previous, nullptr), 0);

  EXPECT_EQ(launched->status(), ChildStatus::unknowable);
}

TEST(PosixChild, GivesTheChildAnUnblockedSignalMask) {
  sigset_t blocked;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGTERM);
  sigset_t previous;
  ASSERT_EQ(::pthread_sigmask(SIG_BLOCK, &blocked, &previous), 0);
  auto launched = PosixChild::launch(shell("kill -TERM $$; echo survived"));
  ASSERT_EQ(::pthread_sigmask(SIG_SETMASK, &previous, nullptr), 0);

  ASSERT_TRUE(launched.has_value()) << launched.error().diagnostic;
  run_to_completion(*launched);
  ASSERT_EQ(launched->status(), ChildStatus::exited);
  EXPECT_TRUE(std::holds_alternative<Signaled>(launched->termination()));
}

#if defined(__linux__)
TEST(PosixChild, ForcedNumericPolicyClosesAMarkerOpenedAfterItsActions) {
  constexpr rlim_t lowered_soft_limit = 256U;
  rlimit original_limit{};
  ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &original_limit), 0);
  if (original_limit.rlim_cur <= lowered_soft_limit ||
      original_limit.rlim_max <= lowered_soft_limit) {
    GTEST_SKIP() << "descriptor limits above 256 are required";
  }
  auto lowered_limit = original_limit;
  lowered_limit.rlim_cur = lowered_soft_limit;
  ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &lowered_limit), 0);

  LateMarker marker;
  pending_marker = &marker;
  std::thread opener{[&marker] { marker.open_after_policy(); }};
  libtmux::detail::force_numeric_spawn_descriptor_policy_for_test(
      finish_descriptor_policy);

  auto launched = PosixChild::launch(shell("printf intended; sleep 30"));
  libtmux::detail::clear_spawn_descriptor_policy_test_override();
  marker.cancel();
  opener.join();
  pending_marker = nullptr;
  const auto restore_result = ::setrlimit(RLIMIT_NOFILE, &original_limit);

  const bool setup_valid = marker.policy_error == 0 && marker.error == 0 &&
                           marker.descriptors[0] >= 3 && marker.descriptors[1] >= 3 &&
                           marker.descriptors[1] < static_cast<int>(lowered_soft_limit);
  if (!setup_valid || !launched.has_value()) {
    if (launched.has_value()) {
      launched->signal_group(SIGKILL);
      run_to_completion(*launched);
    }
    for (const auto descriptor : marker.descriptors) {
      if (descriptor >= 0) {
        static_cast<void>(::close(descriptor));
      }
    }
    ASSERT_EQ(restore_result, 0);
    ASSERT_EQ(marker.policy_error, 0);
    ASSERT_EQ(marker.error, 0);
    ASSERT_GE(marker.descriptors[0], 3);
    ASSERT_GE(marker.descriptors[1], 3);
    ASSERT_LT(marker.descriptors[1], static_cast<int>(lowered_soft_limit));
    ASSERT_TRUE(launched.has_value()) << launched.error().diagnostic;
    return;
  }
  ASSERT_EQ(restore_result, 0);
  EXPECT_EQ(::close(marker.descriptors[0]), 0);

  pollfd output{.fd = launched->descriptor(ChildStream::stdout_stream),
                .events = POLLIN,
                .revents = 0};
  const auto output_ready = ::poll(&output, 1, 2000);
  if (output_ready == 1) {
    static_cast<void>(launched->drain_once(ChildStream::stdout_stream,
                                           ChildClock::now() + std::chrono::seconds{1},
                                           libtmux::DeliveryStatus::indeterminate));
  }
  pollfd inherited{.fd = marker.descriptors[1], .events = POLLOUT, .revents = 0};
  const auto marker_ready = ::poll(&inherited, 1, 1000);

  launched->signal_group(SIGKILL);
  run_to_completion(*launched);
  const auto capture = launched->take_capture();
  static_cast<void>(::close(marker.descriptors[1]));

  ASSERT_EQ(output_ready, 1);
  EXPECT_EQ(text(capture.stdout_bytes), "intended");
  ASSERT_EQ(marker_ready, 1);
  EXPECT_NE(inherited.revents & POLLERR, 0);
}

#if defined(__GLIBC__)
TEST(PosixChild, ForcedNumericPolicyRefusesAMarkerAboveALoweredSoftLimit) {
  constexpr rlim_t lowered_soft_limit = 256U;
  constexpr int minimum_marker_descriptor = 512;
  rlimit original_limit{};
  ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &original_limit), 0);
  if (original_limit.rlim_max == RLIM_INFINITY ||
      original_limit.rlim_max <= static_cast<rlim_t>(minimum_marker_descriptor) ||
      original_limit.rlim_cur <= static_cast<rlim_t>(minimum_marker_descriptor)) {
    GTEST_SKIP() << "a finite descriptor hard limit above 512 is required";
  }

  std::array<int, 2> marker{-1, -1};
  ASSERT_EQ(::pipe(marker.data()), 0);
  const auto high_marker = ::fcntl(marker[0], F_DUPFD, minimum_marker_descriptor);
  ASSERT_GE(high_marker, minimum_marker_descriptor);
  ASSERT_EQ(::close(marker[0]), 0);
  marker[0] = high_marker;

  auto lowered_limit = original_limit;
  lowered_limit.rlim_cur = lowered_soft_limit;
  ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &lowered_limit), 0);
  LateMarker late_marker;
  pending_marker = &late_marker;
  std::thread opener{[&late_marker] { late_marker.open_after_policy(); }};
  libtmux::detail::force_numeric_spawn_descriptor_policy_for_test(
      finish_descriptor_policy);
  auto launched = PosixChild::launch(shell("printf intended; sleep 30"));
  libtmux::detail::clear_spawn_descriptor_policy_test_override();
  late_marker.cancel();
  opener.join();
  pending_marker = nullptr;
  const auto restore_result = ::setrlimit(RLIMIT_NOFILE, &original_limit);

  ASSERT_EQ(restore_result, 0);
  ASSERT_GT(marker[0], static_cast<int>(lowered_soft_limit));
  EXPECT_EQ(::close(marker[0]), 0);
  EXPECT_EQ(::close(marker[1]), 0);
  ASSERT_FALSE(launched.has_value());
  EXPECT_EQ(late_marker.policy_error, EBADF);
  EXPECT_EQ(late_marker.descriptors, (std::array<int, 2>{-1, -1}));
  EXPECT_EQ(launched.error().kind, ProcessError::Kind::pipe);
  EXPECT_EQ(launched.error().delivery, libtmux::DeliveryStatus::not_started);
}
#endif
#endif

// Redaction is a property of the request as it is shown, so it has to hold
// wherever a diagnostic is built from one.
TEST(PosixChild, KeepsASecretArgumentOutOfItsDiagnostic) {
  ProcessRequest request;
  request.executable = "/nonexistent/tmux";
  request.arguments = {{"set-option"},
                       {"hunter2", libtmux::ArgumentSensitivity::secret}};
  auto launched = PosixChild::launch(request);
  ASSERT_FALSE(launched.has_value());
  EXPECT_EQ(launched.error().diagnostic.find("hunter2"), std::string::npos);
  EXPECT_NE(launched.error().diagnostic.find("[REDACTED]"), std::string::npos);
}

} // namespace
