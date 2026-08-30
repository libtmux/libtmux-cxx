#include "process_engine.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include <poll.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

enum class Fault : unsigned int {
  none = 0U,
  poll = 1U << 0U,
  wake_read = 1U << 1U,
  child_read = 1U << 2U,
  wake_write = 1U << 3U,
  status = 1U << 4U,
  signal = 1U << 5U,
  final_reap = 1U << 6U,
  actions_teardown = 1U << 7U,
  attributes_teardown = 1U << 8U,
};

std::atomic<unsigned int> armed_faults{0U};
std::atomic<unsigned int> final_reaps{0U};

[[nodiscard]] constexpr unsigned int bit(Fault fault) noexcept {
  return static_cast<unsigned int>(fault);
}

void arm(std::initializer_list<Fault> faults) {
  unsigned int mask = 0U;
  for (const auto fault : faults) {
    mask |= bit(fault);
  }
  armed_faults.store(mask, std::memory_order_release);
}

void arm(Fault fault) { arm({fault}); }

[[nodiscard]] bool is_armed(Fault fault) noexcept {
  return (armed_faults.load(std::memory_order_acquire) & bit(fault)) != 0U;
}

[[nodiscard]] bool consume(Fault fault) noexcept {
  auto current = armed_faults.load(std::memory_order_acquire);
  const auto wanted = bit(fault);
  while ((current & wanted) != 0U) {
    if (armed_faults.compare_exchange_weak(current, current & ~wanted,
                                           std::memory_order_acq_rel)) {
      return true;
    }
  }
  return false;
}

libtmux::detail::ProcessRequest
shell(std::string script, std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
  libtmux::detail::ProcessRequest request;
  request.executable = "/bin/sh";
  request.arguments = {{"-c"}, {std::move(script)}};
  request.timeout = timeout;
  return request;
}

[[nodiscard]] std::string expected_diagnostic(std::string_view operation,
                                              std::string_view script) {
  return std::string{operation} + " failure running /bin/sh -c " + std::string{script} +
         ": " + std::error_code{EIO, std::generic_category()}.message();
}

void expect_pipe_failure(
    const libtmux::detail::OperationResult<libtmux::detail::ProcessReply>& answer,
    libtmux::DeliveryStatus delivery, std::string_view operation,
    std::string_view script) {
  ASSERT_FALSE(answer.has_value());
  EXPECT_EQ(answer.error().kind, libtmux::FailureKind::pipe);
  EXPECT_EQ(answer.error().delivery, delivery);
  EXPECT_EQ(answer.error().exit_code, -1);
  EXPECT_EQ(answer.error().diagnostic, expected_diagnostic(operation, script));
}

[[nodiscard]] bool wait_for_marker(const std::filesystem::path& marker) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (!std::filesystem::exists(marker) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return std::filesystem::exists(marker);
}

[[nodiscard]] std::string unused_marker() {
  std::string marker = "/tmp/libtmux-engine-failure-XXXXXX";
  const int descriptor = ::mkstemp(marker.data());
  if (descriptor < 0) {
    return {};
  }
  static_cast<void>(::close(descriptor));
  std::error_code ignored;
  static_cast<void>(std::filesystem::remove(marker, ignored));
  return marker;
}

void expect_global_failure(Fault fault, std::string_view operation) {
  auto engine = libtmux::detail::ProcessEngine::start();
  ASSERT_TRUE(engine.has_value()) << engine.error().diagnostic;

  const auto first_marker = unused_marker();
  const auto second_marker = unused_marker();
  ASSERT_FALSE(first_marker.empty());
  ASSERT_FALSE(second_marker.empty());
  const auto first_script = ": > " + first_marker + "; sleep 30";
  const auto second_script = ": > " + second_marker + "; sleep 30";
  auto first = (*engine)->submit(shell(first_script));
  auto second = (*engine)->submit(shell(second_script));
  ASSERT_TRUE(wait_for_marker(first_marker));
  ASSERT_TRUE(wait_for_marker(second_marker));

  arm(fault);
  ASSERT_TRUE(first.request_cancel());
  auto first_answer = libtmux::detail::sync_wait(std::move(first));
  auto second_answer = libtmux::detail::sync_wait(std::move(second));

  expect_pipe_failure(first_answer, libtmux::DeliveryStatus::indeterminate, operation,
                      first_script);
  expect_pipe_failure(second_answer, libtmux::DeliveryStatus::indeterminate, operation,
                      second_script);
  EXPECT_EQ(armed_faults.load(std::memory_order_acquire), 0U);

  constexpr std::string_view later_script{"true"};
  auto later =
      libtmux::detail::sync_wait((*engine)->submit(shell(std::string{later_script})));
  expect_pipe_failure(later, libtmux::DeliveryStatus::not_started, operation,
                      later_script);

  std::error_code ignored;
  static_cast<void>(std::filesystem::remove(first_marker, ignored));
  static_cast<void>(std::filesystem::remove(second_marker, ignored));
}

void expect_sync_async_parity(Fault fault, std::string_view operation) {
  constexpr std::string_view script{"sleep 0.05; printf answer"};
  arm(fault);
  auto synchronous = libtmux::detail::run_process(shell(std::string{script}));
  ASSERT_FALSE(synchronous.has_value());
  EXPECT_EQ(synchronous.error().kind, libtmux::detail::ProcessError::Kind::pipe);
  EXPECT_EQ(synchronous.error().delivery, libtmux::DeliveryStatus::indeterminate);
  EXPECT_EQ(synchronous.error().diagnostic, expected_diagnostic(operation, script));
  ASSERT_EQ(armed_faults.load(std::memory_order_acquire), 0U);

  auto engine = libtmux::detail::ProcessEngine::start();
  ASSERT_TRUE(engine.has_value()) << engine.error().diagnostic;
  arm(fault);
  auto asynchronous =
      libtmux::detail::sync_wait((*engine)->submit(shell(std::string{script})));

  expect_pipe_failure(asynchronous, synchronous.error().delivery, operation, script);
  if (asynchronous.has_value()) {
    return;
  }
  EXPECT_EQ(asynchronous.error().diagnostic, synchronous.error().diagnostic);
  EXPECT_EQ(armed_faults.load(std::memory_order_acquire), 0U);
}

class ProcessEngineFailure : public ::testing::Test {
protected:
  void TearDown() override {
    armed_faults.store(0U, std::memory_order_release);
    final_reaps.store(0U, std::memory_order_release);
  }
};

} // namespace

#if defined(__linux__)
extern "C" {
int __real_poll(struct pollfd*, nfds_t, int);
ssize_t __real_read(int, void*, std::size_t);
ssize_t __real_write(int, const void*, std::size_t);
pid_t __real_waitpid(pid_t, int*, int);
int __real_kill(pid_t, int);
int __real_posix_spawn_file_actions_destroy(posix_spawn_file_actions_t*);
int __real_posix_spawnattr_destroy(posix_spawnattr_t*);

int __wrap_poll(struct pollfd* descriptors, nfds_t count, int timeout) {
  if (count > 1U && consume(Fault::poll)) {
    errno = EIO;
    return -1;
  }
  return __real_poll(descriptors, count, timeout);
}

ssize_t __wrap_read(int descriptor, void* buffer, std::size_t size) {
  if ((size == 64U && consume(Fault::wake_read)) ||
      (size == 16384U && consume(Fault::child_read))) {
    errno = EIO;
    return -1;
  }
  return __real_read(descriptor, buffer, size);
}

ssize_t __wrap_write(int descriptor, const void* buffer, std::size_t size) {
  if (size == 1U && consume(Fault::wake_write)) {
    errno = EIO;
    return -1;
  }
  return __real_write(descriptor, buffer, size);
}

pid_t __wrap_waitpid(pid_t pid, int* status, int options) {
  if ((options & WNOHANG) != 0 && consume(Fault::status)) {
    errno = EIO;
    return -1;
  }
  if (options == 0 && is_armed(Fault::final_reap)) {
    const auto result = __real_waitpid(pid, status, options);
    if (result == pid && consume(Fault::final_reap)) {
      final_reaps.fetch_add(1U, std::memory_order_relaxed);
      errno = EIO;
      return -1;
    }
    return result;
  }
  return __real_waitpid(pid, status, options);
}

int __wrap_kill(pid_t pid, int signal_number) {
  if (pid < 0 && signal_number == SIGTERM && consume(Fault::signal)) {
    errno = EIO;
    return -1;
  }
  return __real_kill(pid, signal_number);
}

int __wrap_posix_spawn_file_actions_destroy(posix_spawn_file_actions_t* actions) {
  const int result = __real_posix_spawn_file_actions_destroy(actions);
  return consume(Fault::actions_teardown) ? EIO : result;
}

int __wrap_posix_spawnattr_destroy(posix_spawnattr_t* attributes) {
  const int result = __real_posix_spawnattr_destroy(attributes);
  return consume(Fault::attributes_teardown) ? EIO : result;
}
}

TEST_F(ProcessEngineFailure, PropagatesWakeWriteFailureToAcceptedWork) {
  expect_global_failure(Fault::wake_write, "engine wake write");
}

TEST_F(ProcessEngineFailure, PropagatesWakeDrainFailureToAcceptedWork) {
  expect_global_failure(Fault::wake_read, "engine wake read");
}

TEST_F(ProcessEngineFailure, PollMatchesTheSynchronousRunner) {
  expect_sync_async_parity(Fault::poll, "pipe poll");
}

TEST_F(ProcessEngineFailure, ChildDrainMatchesTheSynchronousRunner) {
  expect_sync_async_parity(Fault::child_read, "pipe read");
}

TEST_F(ProcessEngineFailure, ChildStatusMatchesTheSynchronousRunner) {
  expect_sync_async_parity(Fault::status, "waitpid pipe");
}

TEST_F(ProcessEngineFailure, ActionTeardownMatchesTheSynchronousRunner) {
  expect_sync_async_parity(Fault::actions_teardown, "pipe");
}

TEST_F(ProcessEngineFailure, AttributeTeardownMatchesTheSynchronousRunner) {
  expect_sync_async_parity(Fault::attributes_teardown, "pipe");
}

TEST_F(ProcessEngineFailure, PropagatesSignalFailure) {
  constexpr std::string_view script{"trap '' TERM; sleep 30"};
  auto engine = libtmux::detail::ProcessEngine::start();
  ASSERT_TRUE(engine.has_value()) << engine.error().diagnostic;
  arm(Fault::signal);

  auto answer = libtmux::detail::sync_wait(
      (*engine)->submit(shell(std::string{script}, std::chrono::milliseconds{200})));

  expect_pipe_failure(answer, libtmux::DeliveryStatus::indeterminate, "kill", script);
  EXPECT_EQ(armed_faults.load(std::memory_order_acquire), 0U);
}

TEST_F(ProcessEngineFailure, PropagatesFinalReapFailure) {
  constexpr std::string_view script{"trap '' TERM; sleep 30"};
  auto engine = libtmux::detail::ProcessEngine::start();
  ASSERT_TRUE(engine.has_value()) << engine.error().diagnostic;
  arm(Fault::final_reap);

  auto answer = libtmux::detail::sync_wait(
      (*engine)->submit(shell(std::string{script}, std::chrono::milliseconds{200})));

  expect_pipe_failure(answer, libtmux::DeliveryStatus::indeterminate, "waitpid pipe",
                      script);
  EXPECT_EQ(armed_faults.load(std::memory_order_acquire), 0U);
  EXPECT_EQ(final_reaps.load(std::memory_order_acquire), 1U);
}

TEST_F(ProcessEngineFailure, CleanupDoesNotOverwriteTheCausalFailure) {
  constexpr std::string_view script{"printf answer; trap '' TERM; sleep 30"};
  auto engine = libtmux::detail::ProcessEngine::start();
  ASSERT_TRUE(engine.has_value()) << engine.error().diagnostic;
  arm({Fault::child_read, Fault::final_reap});

  auto answer =
      libtmux::detail::sync_wait((*engine)->submit(shell(std::string{script})));

  expect_pipe_failure(answer, libtmux::DeliveryStatus::indeterminate, "pipe read",
                      script);
  EXPECT_EQ(armed_faults.load(std::memory_order_acquire), 0U);
  EXPECT_EQ(final_reaps.load(std::memory_order_acquire), 1U);
}
#endif

TEST_F(ProcessEngineFailure, PostExitDrainUsesItsOwnDeadline) {
  constexpr std::string_view script{"sleep 1 &"};
  auto engine = libtmux::detail::ProcessEngine::start();
  ASSERT_TRUE(engine.has_value()) << engine.error().diagnostic;

  const auto started = std::chrono::steady_clock::now();
  auto answer =
      libtmux::detail::sync_wait((*engine)->submit(shell(std::string{script})));
  const auto elapsed = std::chrono::steady_clock::now() - started;

  ASSERT_TRUE(answer.has_value()) << answer.error().diagnostic;
  EXPECT_GE(elapsed, std::chrono::milliseconds{75});
  EXPECT_LT(elapsed, std::chrono::milliseconds{300});
}

TEST_F(ProcessEngineFailure, ConcurrentCloseSharesOneTerminalState) {
  auto engine = libtmux::detail::ProcessEngine::start();
  ASSERT_TRUE(engine.has_value()) << engine.error().diagnostic;
  const auto marker = unused_marker();
  ASSERT_FALSE(marker.empty());
  auto running = (*engine)->submit(shell(": > " + marker + "; trap '' TERM; sleep 30"));
  ASSERT_TRUE(wait_for_marker(marker));

  constexpr std::size_t caller_count = 8U;
  std::array<libtmux::detail::EngineShutdown, caller_count> reports{};
  std::array<std::exception_ptr, caller_count> failures{};
  std::array<std::thread, caller_count> callers;
  std::atomic<std::size_t> ready{0U};
  std::atomic<bool> start{false};
  for (std::size_t index = 0U; index < caller_count; ++index) {
    callers[index] = std::thread{[&, index] {
      ready.fetch_add(1U, std::memory_order_release);
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      try {
        reports[index] = (*engine)->close();
      } catch (...) {
        failures[index] = std::current_exception();
      }
    }};
  }
  while (ready.load(std::memory_order_acquire) != caller_count) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);
  for (auto& caller : callers) {
    caller.join();
  }

  for (std::size_t index = 0U; index < caller_count; ++index) {
    EXPECT_EQ(failures[index], nullptr);
    EXPECT_EQ(reports[index].operations_published, reports[0].operations_published);
    EXPECT_EQ(reports[index].children_reaped, reports[0].children_reaped);
    EXPECT_EQ(reports[index].complete, reports[0].complete);
  }
  EXPECT_EQ(reports[0].operations_published, 1U);
  EXPECT_EQ(reports[0].children_reaped, 1U);
  EXPECT_TRUE(reports[0].complete);
  auto answer = libtmux::detail::sync_wait(std::move(running));
  EXPECT_FALSE(answer.has_value());

  std::error_code ignored;
  static_cast<void>(std::filesystem::remove(marker, ignored));
}
