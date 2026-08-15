#include "libtmux/expected.hpp"
#include "support/process.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <poll.h>
#include <pthread.h>
#if defined(__linux__)
#include <sys/ptrace.h>
#include <sys/syscall.h>
#endif
#include <sys/wait.h>
#include <unistd.h>

namespace {
#if defined(__linux__)
std::atomic<bool> emulate_continuous_read{false};
enum class PidfdFailureMode { None, Reservation, Child, ChildDelayedReap };
std::atomic<PidfdFailureMode> pidfd_failure_mode{PidfdFailureMode::None};
std::atomic<pid_t> failed_pidfd_child{-1};
std::atomic<pid_t> failed_pidfd_tracer{-1};
std::atomic<int> failed_pidfd_tracer_attach{-ECHILD};
std::atomic<long long> child_pidfd_failure_nanoseconds{0};
std::string delayed_reap_trace;
std::atomic<pid_t> blocked_waitpid{-1};
std::atomic<pid_t> redirected_waitpid{-1};
std::atomic<pid_t> redirected_waitpid_target{-1};

bool wait_for_ptrace_ready(pid_t pid) {
  const auto expected = "ptrace-ready\tPID=" + std::to_string(pid);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (std::chrono::steady_clock::now() < deadline) {
    std::ifstream input{delayed_reap_trace};
    std::string line;
    while (std::getline(input, line)) {
      if (line == expected) {
        return true;
      }
    }
    static_cast<void>(::poll(nullptr, 0, 5));
  }
  return false;
}

void start_pidfd_failure_tracer(pid_t target) {
  std::array<int, 2> status_pipe{-1, -1};
  if (::pipe(status_pipe.data()) != 0) {
    failed_pidfd_tracer_attach.store(-errno, std::memory_order_relaxed);
    return;
  }
  const auto tracer = ::fork();
  if (tracer == 0) {
    static_cast<void>(::close(status_pipe[0]));
    const auto result = ::ptrace(PTRACE_SEIZE, target, nullptr, nullptr);
    const auto reported = result == 0 ? 0 : -errno;
    static_cast<void>(::write(status_pipe[1], &reported, sizeof(reported)));
    static_cast<void>(::close(status_pipe[1]));
    if (reported == 0) {
      static_cast<void>(::poll(nullptr, 0, 800));
    }
    std::_Exit(0);
  }
  static_cast<void>(::close(status_pipe[1]));
  int reported = tracer < 0 ? -errno : -ECHILD;
  if (tracer > 0) {
    ssize_t count = -1;
    do {
      count = ::read(status_pipe[0], &reported, sizeof(reported));
    } while (count < 0 && errno == EINTR);
    if (count != static_cast<ssize_t>(sizeof(reported))) {
      reported = -EIO;
    }
  }
  static_cast<void>(::close(status_pipe[0]));
  failed_pidfd_tracer.store(tracer, std::memory_order_relaxed);
  failed_pidfd_tracer_attach.store(reported, std::memory_order_relaxed);
}
#endif
} // namespace

#if defined(__linux__)
extern "C" ssize_t __real_read(int descriptor, void* buffer, std::size_t size);

extern "C" ssize_t __wrap_read(int descriptor, void* buffer, std::size_t size) {
  if (emulate_continuous_read.load(std::memory_order_relaxed)) {
    std::memset(buffer, 'c', size);
    return static_cast<ssize_t>(size);
  }
  return __real_read(descriptor, buffer, size);
}

extern "C" long __real_syscall(long number, ...);

extern "C" long __wrap_syscall(long number, ...) {
  std::va_list arguments;
  va_start(arguments, number);
  if (number == SYS_pidfd_open) {
    const auto pid = static_cast<pid_t>(va_arg(arguments, int));
    const auto flags = va_arg(arguments, unsigned int);
    va_end(arguments);
    const auto failure_mode = pidfd_failure_mode.load(std::memory_order_relaxed);
    const auto child_failure = failure_mode == PidfdFailureMode::Child ||
                               failure_mode == PidfdFailureMode::ChildDelayedReap;
    if ((failure_mode == PidfdFailureMode::Reservation && pid == ::getpid()) ||
        (child_failure && pid != ::getpid())) {
      if (child_failure) {
        failed_pidfd_child.store(pid, std::memory_order_relaxed);
      }
      if (failure_mode == PidfdFailureMode::ChildDelayedReap) {
        if (wait_for_ptrace_ready(pid)) {
          start_pidfd_failure_tracer(pid);
        } else {
          failed_pidfd_tracer_attach.store(-ETIMEDOUT, std::memory_order_relaxed);
        }
      }
      child_pidfd_failure_nanoseconds.store(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count(),
          std::memory_order_relaxed);
      errno = EMFILE;
      return -1;
    }
    return __real_syscall(number, pid, flags);
  }
  if (number == SYS_pidfd_send_signal) {
    const auto pidfd = va_arg(arguments, int);
    const auto signal_number = va_arg(arguments, int);
    const auto* information = va_arg(arguments, const siginfo_t*);
    const auto flags = va_arg(arguments, unsigned int);
    va_end(arguments);
    return __real_syscall(number, pidfd, signal_number, information, flags);
  }
  // Anything this wrapper does not model must reach the real syscall: one
  // standard library routes its futex waits through here, and answering
  // ENOSYS to those wedges every test that touches a condition variable.
  const auto first = va_arg(arguments, long);
  const auto second = va_arg(arguments, long);
  const auto third = va_arg(arguments, long);
  const auto fourth = va_arg(arguments, long);
  const auto fifth = va_arg(arguments, long);
  const auto sixth = va_arg(arguments, long);
  va_end(arguments);
  return __real_syscall(number, first, second, third, fourth, fifth, sixth);
}

extern "C" pid_t __real_waitpid(pid_t pid, int* status, int options);

extern "C" pid_t __wrap_waitpid(pid_t pid, int* status, int options) {
  if (pid == blocked_waitpid.load(std::memory_order_relaxed)) {
    errno = EINVAL;
    return -1;
  }
  if (pid == redirected_waitpid.load(std::memory_order_relaxed)) {
    const auto target = redirected_waitpid_target.load(std::memory_order_relaxed);
    const auto result = __real_waitpid(target, status, options);
    return result == target ? pid : result;
  }
  return __real_waitpid(pid, status, options);
}
#endif

namespace {

using libtmux::test::detail::ChildProcess;
using libtmux::test::detail::ProcessClock;
using libtmux::test::detail::ProcessOptions;

bool has_pidfd_for(pid_t pid) {
  const auto expected = "Pid:\t" + std::to_string(pid);
  for (const auto& entry : std::filesystem::directory_iterator{"/proc/self/fdinfo"}) {
    std::ifstream input{entry.path()};
    std::string line;
    while (std::getline(input, line)) {
      if (line == expected) {
        return true;
      }
    }
  }
  return false;
}

#if defined(__linux__)
std::optional<int> pidfd_for(pid_t pid) {
  const auto expected = "Pid:\t" + std::to_string(pid);
  for (const auto& entry : std::filesystem::directory_iterator{"/proc/self/fdinfo"}) {
    std::ifstream input{entry.path()};
    std::string line;
    while (std::getline(input, line)) {
      if (line == expected) {
        return std::stoi(entry.path().filename().string());
      }
    }
  }
  return std::nullopt;
}

std::filesystem::path process_trace_path(std::string_view stem) {
  static std::atomic<unsigned long> sequence{0};
  return std::filesystem::temp_directory_path() /
         (std::string{stem} + "-" + std::to_string(::getpid()) + "-" +
          std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
}

pid_t traced_process_pid(const std::filesystem::path& trace,
                         libtmux::test::detail::ProcessClock::time_point deadline) {
  while (libtmux::test::detail::ProcessClock::now() < deadline) {
    std::ifstream input{trace};
    std::string role;
    std::string pid_field;
    if (input >> role >> pid_field && role == "process-probe" &&
        pid_field.starts_with("PID=")) {
      return static_cast<pid_t>(std::stoi(pid_field.substr(4U)));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  return -1;
}

bool process_is_gone(pid_t pid,
                     libtmux::test::detail::ProcessClock::time_point deadline) {
  while (libtmux::test::detail::ProcessClock::now() < deadline) {
    errno = 0;
    if (::kill(pid, 0) < 0 && errno == ESRCH) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  return false;
}

bool child_status_available(int pidfd,
                            libtmux::test::detail::ProcessClock::time_point deadline) {
  while (libtmux::test::detail::ProcessClock::now() < deadline) {
    siginfo_t information{};
    if (::waitid(P_PIDFD, static_cast<id_t>(pidfd), &information,
                 WEXITED | WNOHANG | WNOWAIT) == 0 &&
        information.si_pid != 0) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  return false;
}

void verify_late_reaper_handoff() {
  auto child = ChildProcess::spawn(
      {.executable = LIBTMUX_FAKE_TMUX_PATH,
       .arguments = {"--process-probe", "wait"},
       .environment = libtmux::test::detail::current_environment()});
  if (!child.has_value()) {
    std::_Exit(20);
  }
  const auto pid = child->pid();
  child->terminate_and_reap(ProcessClock::now());

  const auto deadline = ProcessClock::now() + std::chrono::milliseconds{300};
  while (ProcessClock::now() < deadline) {
    siginfo_t information{};
    errno = 0;
    if (::waitid(P_PID, static_cast<id_t>(pid), &information,
                 WEXITED | WNOHANG | WNOWAIT) < 0 &&
        errno == ECHILD) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  std::_Exit(21);
}

class ScopedPidfdFailure final {
public:
  explicit ScopedPidfdFailure(PidfdFailureMode mode, std::filesystem::path trace = {}) {
    failed_pidfd_child.store(-1, std::memory_order_relaxed);
    failed_pidfd_tracer.store(-1, std::memory_order_relaxed);
    failed_pidfd_tracer_attach.store(-ECHILD, std::memory_order_relaxed);
    child_pidfd_failure_nanoseconds.store(0, std::memory_order_relaxed);
    delayed_reap_trace = trace.string();
    pidfd_failure_mode.store(mode, std::memory_order_relaxed);
  }
  ~ScopedPidfdFailure() {
    pidfd_failure_mode.store(PidfdFailureMode::None, std::memory_order_relaxed);
    delayed_reap_trace.clear();
  }
  ScopedPidfdFailure(const ScopedPidfdFailure&) = delete;
  ScopedPidfdFailure& operator=(const ScopedPidfdFailure&) = delete;
};

class ScopedWaitpidBlock final {
public:
  explicit ScopedWaitpidBlock(pid_t pid) {
    blocked_waitpid.store(pid, std::memory_order_relaxed);
  }
  ~ScopedWaitpidBlock() { blocked_waitpid.store(-1, std::memory_order_relaxed); }
  ScopedWaitpidBlock(const ScopedWaitpidBlock&) = delete;
  ScopedWaitpidBlock& operator=(const ScopedWaitpidBlock&) = delete;
};

class ScopedWaitpidRedirect final {
public:
  ScopedWaitpidRedirect(pid_t source, pid_t target) {
    redirected_waitpid_target.store(target, std::memory_order_relaxed);
    redirected_waitpid.store(source, std::memory_order_release);
  }
  ~ScopedWaitpidRedirect() {
    redirected_waitpid.store(-1, std::memory_order_release);
    redirected_waitpid_target.store(-1, std::memory_order_relaxed);
  }
  ScopedWaitpidRedirect(const ScopedWaitpidRedirect&) = delete;
  ScopedWaitpidRedirect& operator=(const ScopedWaitpidRedirect&) = delete;
};
#endif

void handle_interrupt(int /*signal_number*/) {}

TEST(ProcessSupport, CapturesBothStreamsAndReapsDirectChild) {
  auto child = ChildProcess::spawn(
      {.executable = LIBTMUX_FAKE_TMUX_PATH,
       .arguments = {"--process-probe", "streams"},
       .environment = libtmux::test::detail::current_environment()});
  ASSERT_TRUE(child.has_value()) << child.error();
  const auto pid = child->pid();

  EXPECT_TRUE(child->wait_until(ProcessClock::now() + std::chrono::seconds{2}));
  const auto wait_status = child->wait_status().value_or(-1);
  EXPECT_TRUE(WIFEXITED(wait_status));
  EXPECT_EQ(WEXITSTATUS(wait_status), 0);
  EXPECT_EQ(child->stdout_text(), "stdout-probe\n");
  EXPECT_EQ(child->stderr_text(), "stderr-probe\n");

  errno = 0;
  EXPECT_EQ(::waitpid(pid, nullptr, WNOHANG), -1);
  EXPECT_EQ(errno, ECHILD);
}

TEST(ProcessSupport, RejectsMissingExecutableAtSpawn) {
  auto child = ChildProcess::spawn(
      {.executable = "/definitely/not/a/libtmux-test-program",
       .environment = libtmux::test::detail::current_environment()});

  ASSERT_FALSE(child.has_value());
  EXPECT_NE(child.error().find("posix_spawnp"), std::string::npos);
}

TEST(ProcessSupport, BoundsCaptureWhileDrainingLargeDualPipeOutput) {
  constexpr std::size_t capture_limit = 4096;
  auto child =
      ChildProcess::spawn({.executable = LIBTMUX_FAKE_TMUX_PATH,
                           .arguments = {"--process-probe", "large-output"},
                           .environment = libtmux::test::detail::current_environment(),
                           .capture_limit = capture_limit});
  ASSERT_TRUE(child.has_value()) << child.error();

  EXPECT_TRUE(child->wait_until(ProcessClock::now() + std::chrono::seconds{5}));
  EXPECT_EQ(child->stdout_text().size(), capture_limit);
  EXPECT_EQ(child->stderr_text().size(), capture_limit);
}

TEST(ProcessSupport, KeepsStableKernelIdentityUntilDirectChildIsReaped) {
  auto child = ChildProcess::spawn(
      {.executable = LIBTMUX_FAKE_TMUX_PATH,
       .arguments = {"--process-probe", "wait"},
       .environment = libtmux::test::detail::current_environment()});
  ASSERT_TRUE(child.has_value()) << child.error();

  child->close_output();
  EXPECT_TRUE(has_pidfd_for(child->pid()));

  EXPECT_TRUE(child->send_signal(SIGKILL));
  EXPECT_TRUE(child->wait_until(ProcessClock::now() + std::chrono::seconds{2}));
}

#if defined(__linux__)
TEST(ProcessSupport, RejectsBeforeSpawnWhenPidfdCapacityCannotBeReserved) {
  const auto trace = process_trace_path("libtmux-pidfd-reservation");
  auto environment = libtmux::test::detail::current_environment();
  libtmux::test::detail::set_environment(environment, "LIBTMUX_FAKE_TRACE",
                                         trace.string());

  libtmux::expected<ChildProcess, std::string> child = [&] {
    ScopedPidfdFailure failure{PidfdFailureMode::Reservation};
    return ChildProcess::spawn({.executable = LIBTMUX_FAKE_TMUX_PATH,
                                .arguments = {"--process-probe", "wait"},
                                .environment = environment});
  }();
  const auto spawned_pid =
      traced_process_pid(trace, ProcessClock::now() + std::chrono::milliseconds{100});

  if (child.has_value()) {
    static_cast<void>(child->send_signal(SIGKILL));
    static_cast<void>(child->wait_until(ProcessClock::now() + std::chrono::seconds{2}));
  }
  EXPECT_FALSE(child.has_value());
  if (!child.has_value()) {
    EXPECT_NE(child.error().find("pidfd"), std::string::npos);
  }
  EXPECT_EQ(spawned_pid, -1);
  std::error_code error;
  std::filesystem::remove(trace, error);
}

TEST(ProcessSupport, PidfdOpenFailureTerminatesAndReapsNonExitingChild) {
  const auto trace = process_trace_path("libtmux-pidfd-child-failure");
  auto environment = libtmux::test::detail::current_environment();
  libtmux::test::detail::set_environment(environment, "LIBTMUX_FAKE_TRACE",
                                         trace.string());

  const auto started = ProcessClock::now();
  auto child = [&] {
    ScopedPidfdFailure failure{PidfdFailureMode::Child};
    return ChildProcess::spawn({.executable = LIBTMUX_FAKE_TMUX_PATH,
                                .arguments = {"--process-probe", "wait"},
                                .environment = environment});
  }();
  const auto elapsed = ProcessClock::now() - started;
  const auto spawned_pid = failed_pidfd_child.load(std::memory_order_relaxed);

  ASSERT_FALSE(child.has_value());
  EXPECT_NE(child.error().find("pidfd_open"), std::string::npos);
  ASSERT_GT(spawned_pid, 0);
  const auto was_reaped = process_is_gone(
      spawned_pid, ProcessClock::now() + std::chrono::milliseconds{300});
  EXPECT_TRUE(was_reaped);
  EXPECT_LT(elapsed, std::chrono::milliseconds{500});
  if (!was_reaped) {
    static_cast<void>(::kill(spawned_pid, SIGKILL));
    static_cast<void>(
        process_is_gone(spawned_pid, ProcessClock::now() + std::chrono::seconds{2}));
  }
  std::error_code error;
  std::filesystem::remove(trace, error);
}

TEST(ProcessSupport, PidfdOpenFailureRetainsDelayedReapOwnership) {
  const auto trace = process_trace_path("libtmux-pidfd-delayed-reap");
  auto environment = libtmux::test::detail::current_environment();
  libtmux::test::detail::set_environment(environment, "LIBTMUX_FAKE_TRACE",
                                         trace.string());

  auto child = [&] {
    ScopedPidfdFailure failure{PidfdFailureMode::ChildDelayedReap, trace};
    return ChildProcess::spawn({.executable = LIBTMUX_FAKE_TMUX_PATH,
                                .arguments = {"--process-probe", "ptrace-wait"},
                                .environment = environment});
  }();
  const auto returned_nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  const auto failure_nanoseconds =
      child_pidfd_failure_nanoseconds.load(std::memory_order_relaxed);
  const auto spawned_pid = failed_pidfd_child.load(std::memory_order_relaxed);
  const auto tracer_pid = failed_pidfd_tracer.load(std::memory_order_relaxed);

  ASSERT_FALSE(child.has_value());
  EXPECT_NE(child.error().find("direct-child cleanup exceeded"), std::string::npos);
  ASSERT_GT(spawned_pid, 0);
  ASSERT_GT(tracer_pid, 0);
  ASSERT_EQ(failed_pidfd_tracer_attach.load(std::memory_order_relaxed), 0);
  ASSERT_GT(failure_nanoseconds, 0);
  EXPECT_LT(std::chrono::nanoseconds{returned_nanoseconds - failure_nanoseconds},
            std::chrono::milliseconds{300});

  int tracer_status = 0;
  const auto tracer_probe = ::waitpid(tracer_pid, &tracer_status, WNOHANG);
  EXPECT_EQ(tracer_probe, 0);
  if (tracer_probe == 0) {
    while (::waitpid(tracer_pid, &tracer_status, 0) < 0 && errno == EINTR) {
    }
  }

  const auto was_reaped = process_is_gone(
      spawned_pid, ProcessClock::now() + std::chrono::milliseconds{300});
  EXPECT_TRUE(was_reaped);
  if (!was_reaped) {
    static_cast<void>(::waitpid(spawned_pid, nullptr, WNOHANG));
  }
  std::error_code error;
  std::filesystem::remove(trace, error);
}

TEST(ProcessSupport, WaitsAndPreservesStatusWithoutNumericPidReaping) {
  auto child = ChildProcess::spawn(
      {.executable = LIBTMUX_FAKE_TMUX_PATH,
       .arguments = {"--process-probe", "streams"},
       .environment = libtmux::test::detail::current_environment()});
  ASSERT_TRUE(child.has_value()) << child.error();

  {
    ScopedWaitpidBlock block{child->pid()};
    EXPECT_TRUE(
        child->wait_until(ProcessClock::now() + std::chrono::milliseconds{300}));
  }
  const auto status = child->wait_status().value_or(-1);
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(ProcessSupport, PreservesSignalStatusWithoutNumericPidReaping) {
  auto child = ChildProcess::spawn(
      {.executable = LIBTMUX_FAKE_TMUX_PATH,
       .arguments = {"--process-probe", "wait"},
       .environment = libtmux::test::detail::current_environment()});
  ASSERT_TRUE(child.has_value()) << child.error();

  {
    ScopedWaitpidBlock block{child->pid()};
    ASSERT_TRUE(child->send_signal(SIGKILL));
    EXPECT_TRUE(
        child->wait_until(ProcessClock::now() + std::chrono::milliseconds{300}));
  }
  const auto status = child->wait_status().value_or(-1);
  EXPECT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(WTERMSIG(status), SIGKILL);
}

TEST(ProcessSupport, AmbientPidfdReapCannotRedirectToReusedNumericPid) {
  auto original = ChildProcess::spawn(
      {.executable = LIBTMUX_FAKE_TMUX_PATH,
       .arguments = {"--process-probe", "streams"},
       .environment = libtmux::test::detail::current_environment()});
  ASSERT_TRUE(original.has_value()) << original.error();
  const auto original_pidfd = pidfd_for(original->pid());
  ASSERT_TRUE(original_pidfd.has_value());
  ASSERT_TRUE(child_status_available(*original_pidfd,
                                     ProcessClock::now() + std::chrono::seconds{2}));
  siginfo_t original_status{};
  ASSERT_EQ(::waitid(P_PIDFD, static_cast<id_t>(*original_pidfd), &original_status,
                     WEXITED | WNOHANG),
            0);
  ASSERT_EQ(original_status.si_pid, original->pid());

  auto replacement = ChildProcess::spawn(
      {.executable = LIBTMUX_FAKE_TMUX_PATH,
       .arguments = {"--process-probe", "streams"},
       .environment = libtmux::test::detail::current_environment()});
  ASSERT_TRUE(replacement.has_value()) << replacement.error();
  const auto replacement_pidfd = pidfd_for(replacement->pid());
  ASSERT_TRUE(replacement_pidfd.has_value());
  ASSERT_TRUE(child_status_available(*replacement_pidfd,
                                     ProcessClock::now() + std::chrono::seconds{2}));

  {
    ScopedWaitpidRedirect redirect{original->pid(), replacement->pid()};
    EXPECT_FALSE(original->is_running());
  }

  siginfo_t replacement_status{};
  errno = 0;
  EXPECT_EQ(::waitid(P_PIDFD, static_cast<id_t>(*replacement_pidfd),
                     &replacement_status, WEXITED | WNOHANG),
            0);
  EXPECT_EQ(replacement_status.si_pid, replacement->pid());
}

TEST(ProcessSupport, BackgroundHandoffReapsWithoutNumericPidWait) {
  auto child = ChildProcess::spawn(
      {.executable = LIBTMUX_FAKE_TMUX_PATH,
       .arguments = {"--process-probe", "wait"},
       .environment = libtmux::test::detail::current_environment()});
  ASSERT_TRUE(child.has_value()) << child.error();
  const auto pid = child->pid();

  {
    ScopedWaitpidBlock block{pid};
    static_cast<void>(child->send_signal(SIGKILL));
    child->terminate_and_reap(ProcessClock::now());
    siginfo_t information{};
    bool reaped = false;
    const auto deadline = ProcessClock::now() + std::chrono::milliseconds{300};
    while (ProcessClock::now() < deadline) {
      errno = 0;
      if (::waitid(P_PID, static_cast<id_t>(pid), &information,
                   WEXITED | WNOHANG | WNOWAIT) < 0 &&
          errno == ECHILD) {
        reaped = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    EXPECT_TRUE(reaped);
  }
  static_cast<void>(
      process_is_gone(pid, ProcessClock::now() + std::chrono::seconds{2}));
}

TEST(ProcessSupport, RejectsIgnoredSigchldBeforeSpawn) {
  struct sigaction previous_action {};
  struct sigaction ignored_action {};
  ignored_action.sa_handler = SIG_IGN;
  sigemptyset(&ignored_action.sa_mask);
  ASSERT_EQ(::sigaction(SIGCHLD, &ignored_action, &previous_action), 0);
  auto child = ChildProcess::spawn(
      {.executable = LIBTMUX_FAKE_TMUX_PATH,
       .arguments = {"--process-probe", "wait"},
       .environment = libtmux::test::detail::current_environment()});
  ASSERT_EQ(::sigaction(SIGCHLD, &previous_action, nullptr), 0);

  if (child.has_value()) {
    static_cast<void>(child->send_signal(SIGKILL));
    static_cast<void>(child->wait_until(ProcessClock::now() + std::chrono::seconds{2}));
  }
  ASSERT_FALSE(child.has_value());
  EXPECT_NE(child.error().find("SIGCHLD"), std::string::npos);
}

TEST(ProcessSupport, RejectsNoCldwaitBeforeSpawn) {
  struct sigaction previous_action {};
  struct sigaction no_wait_action {};
  no_wait_action.sa_handler = SIG_DFL;
  no_wait_action.sa_flags = SA_NOCLDWAIT;
  sigemptyset(&no_wait_action.sa_mask);
  ASSERT_EQ(::sigaction(SIGCHLD, &no_wait_action, &previous_action), 0);
  auto child = ChildProcess::spawn(
      {.executable = LIBTMUX_FAKE_TMUX_PATH,
       .arguments = {"--process-probe", "wait"},
       .environment = libtmux::test::detail::current_environment()});
  ASSERT_EQ(::sigaction(SIGCHLD, &previous_action, nullptr), 0);

  if (child.has_value()) {
    static_cast<void>(child->send_signal(SIGKILL));
    static_cast<void>(child->wait_until(ProcessClock::now() + std::chrono::seconds{2}));
  }
  ASSERT_FALSE(child.has_value());
  EXPECT_NE(child.error().find("SIGCHLD"), std::string::npos);
}

TEST(ProcessSupport, ReaperRemainsAvailableForLateExitHandoff) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_EXIT(
      {
        if (std::atexit(verify_late_reaper_handoff) != 0) {
          std::_Exit(22);
        }
        auto child = ChildProcess::spawn(
            {.executable = LIBTMUX_FAKE_TMUX_PATH,
             .arguments = {"--process-probe", "streams"},
             .environment = libtmux::test::detail::current_environment()});
        if (!child.has_value() ||
            !child->wait_until(ProcessClock::now() + std::chrono::seconds{2})) {
          std::_Exit(23);
        }
        std::exit(0);
      },
      ::testing::ExitedWithCode(0), "");
}
#endif

#if defined(__linux__)
TEST(ProcessSupport, ContinuousOutputCannotEscapeDrainDeadline) {
  auto child = ChildProcess::spawn(
      {.executable = LIBTMUX_FAKE_TMUX_PATH,
       .arguments = {"--process-probe", "continuous-output"},
       .environment = libtmux::test::detail::current_environment()});
  ASSERT_TRUE(child.has_value()) << child.error();

  emulate_continuous_read.store(true, std::memory_order_relaxed);
  std::thread release_reader{[] {
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    emulate_continuous_read.store(false, std::memory_order_relaxed);
  }};
  const auto started = ProcessClock::now();
  child->drain_until(started + std::chrono::milliseconds{40});
  const auto elapsed = ProcessClock::now() - started;
  release_reader.join();

  EXPECT_LT(elapsed, std::chrono::milliseconds{150});
  static_cast<void>(child->send_signal(SIGKILL));
  EXPECT_TRUE(child->wait_until(ProcessClock::now() + std::chrono::seconds{2}));
}
#endif

TEST(ProcessSupport, RepeatedEintrCannotExtendWaitDeadline) {
  auto child = ChildProcess::spawn(
      {.executable = LIBTMUX_FAKE_TMUX_PATH,
       .arguments = {"--process-probe", "wait"},
       .environment = libtmux::test::detail::current_environment()});
  ASSERT_TRUE(child.has_value()) << child.error();

  struct sigaction action {};
  struct sigaction previous_action {};
  action.sa_handler = handle_interrupt;
  sigemptyset(&action.sa_mask);
  ASSERT_EQ(::sigaction(SIGUSR1, &action, &previous_action), 0);

  const auto receiver = ::pthread_self();
  std::thread interrupter{[receiver] {
    for (int attempt = 0; attempt < 200; ++attempt) {
      static_cast<void>(::pthread_kill(receiver, SIGUSR1));
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
  }};

  const auto started = ProcessClock::now();
  EXPECT_FALSE(child->wait_until(started + std::chrono::milliseconds{40}));
  const auto elapsed = ProcessClock::now() - started;

  interrupter.join();
  EXPECT_EQ(::sigaction(SIGUSR1, &previous_action, nullptr), 0);
  EXPECT_LT(elapsed, std::chrono::milliseconds{150});
  static_cast<void>(child->send_signal(SIGKILL));
  EXPECT_TRUE(child->wait_until(ProcessClock::now() + std::chrono::seconds{2}));
}

TEST(ProcessSupport, DoesNotWaitForEscapedDescriptorHolder) {
  auto child = ChildProcess::spawn(
      {.executable = LIBTMUX_FAKE_TMUX_PATH,
       .arguments = {"--process-probe", "escaped-holder"},
       .environment = libtmux::test::detail::current_environment()});
  ASSERT_TRUE(child.has_value()) << child.error();

  const auto started = ProcessClock::now();
  EXPECT_TRUE(child->wait_until(started + std::chrono::milliseconds{300}));
  child->close_output();
  EXPECT_LT(ProcessClock::now() - started, std::chrono::milliseconds{300});
}

TEST(ProcessSupport, MoveAssignmentReapsThePreviouslyOwnedChild) {
  auto first = ChildProcess::spawn(
      {.executable = LIBTMUX_FAKE_TMUX_PATH,
       .arguments = {"--process-probe", "wait"},
       .environment = libtmux::test::detail::current_environment()});
  auto second = ChildProcess::spawn(
      {.executable = LIBTMUX_FAKE_TMUX_PATH,
       .arguments = {"--process-probe", "streams"},
       .environment = libtmux::test::detail::current_environment()});
  ASSERT_TRUE(first.has_value()) << first.error();
  ASSERT_TRUE(second.has_value()) << second.error();
  const auto first_pid = first->pid();

  *first = std::move(*second);

  errno = 0;
  EXPECT_EQ(::waitpid(first_pid, nullptr, WNOHANG), -1);
  EXPECT_EQ(errno, ECHILD);
  EXPECT_TRUE(first->wait_until(ProcessClock::now() + std::chrono::seconds{2}));
}

} // namespace
