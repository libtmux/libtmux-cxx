// The asynchronous engine against real processes: two threads, whatever the
// load, and an answer only once the child has exited and its output has ended.
#include "process_engine.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <semaphore>
#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {

using libtmux::detail::Exited;
using libtmux::detail::Operation;
using libtmux::detail::ProcessEngine;
using libtmux::detail::ProcessReply;
using libtmux::detail::ProcessRequest;
using libtmux::detail::Signaled;
using libtmux::detail::sync_wait;

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

class ScopedPathRemoval final {
public:
  explicit ScopedPathRemoval(std::filesystem::path path) noexcept
      : path_{std::move(path)} {}

  ~ScopedPathRemoval() {
    std::error_code ignored;
    static_cast<void>(std::filesystem::remove(path_, ignored));
  }

  ScopedPathRemoval(const ScopedPathRemoval&) = delete;
  ScopedPathRemoval& operator=(const ScopedPathRemoval&) = delete;

private:
  std::filesystem::path path_;
};

class ScopedEngineClose final {
public:
  explicit ScopedEngineClose(std::shared_ptr<ProcessEngine> engine) noexcept
      : engine_{std::move(engine)} {}

  ~ScopedEngineClose() { close(); }

  ScopedEngineClose(const ScopedEngineClose&) = delete;
  ScopedEngineClose& operator=(const ScopedEngineClose&) = delete;

  void close() {
    if (engine_) {
      static_cast<void>(engine_->close());
      engine_.reset();
    }
  }

private:
  std::shared_ptr<ProcessEngine> engine_;
};

struct RetirementObservation final {
  std::atomic<int> calls{0};
  std::atomic<int> status_result{0};
  std::atomic<int> status_error{0};
  std::atomic_bool ready{false};
};

class LaunchShutdownBarrier final {
public:
  void hold_launch() {
    launch_withheld_.release();
    continue_launch_.acquire();
  }

  void observe(libtmux::detail::EngineReactorEvent event) {
    const auto event_index = event_count_.fetch_add(1U, std::memory_order_relaxed);
    if (event_index != 0U) {
      return;
    }
    first_event_.store(event, std::memory_order_relaxed);
    event_reached_.release();
    if (event == libtmux::detail::EngineReactorEvent::waiting_for_launch) {
      continue_reactor_.acquire();
    }
  }

  [[nodiscard]] bool wait_until_launch_is_withheld() {
    return launch_withheld_.try_acquire_for(std::chrono::seconds{2});
  }

  [[nodiscard]] std::optional<libtmux::detail::EngineReactorEvent>
  wait_for_reactor_event() {
    if (!event_reached_.try_acquire_for(std::chrono::seconds{2})) {
      return std::nullopt;
    }
    return first_event_.load(std::memory_order_relaxed);
  }

  void release() {
    continue_reactor_.release();
    continue_launch_.release();
  }

  [[nodiscard]] std::size_t event_count() const noexcept {
    return event_count_.load(std::memory_order_relaxed);
  }

private:
  std::binary_semaphore launch_withheld_{0};
  std::binary_semaphore event_reached_{0};
  std::binary_semaphore continue_reactor_{0};
  std::binary_semaphore continue_launch_{0};
  std::atomic<libtmux::detail::EngineReactorEvent> first_event_{
      libtmux::detail::EngineReactorEvent::exited};
  std::atomic_size_t event_count_{0U};
};

TEST(ProcessEngine, AnswersOneProcessThroughSyncWait) {
  auto engine = ProcessEngine::start();
  ASSERT_TRUE(engine.has_value()) << engine.error().diagnostic;

  auto reply = sync_wait((*engine)->submit(shell("printf out; printf err >&2")));

  ASSERT_TRUE(reply.has_value()) << reply.error().diagnostic;
  EXPECT_EQ(text(reply->stdout_bytes), "out");
  EXPECT_EQ(text(reply->stderr_bytes), "err");
  ASSERT_TRUE(std::holds_alternative<Exited>(reply->termination));
  EXPECT_EQ(std::get<Exited>(reply->termination).code, 0);
}

TEST(ProcessEngine, TransportRetirementFiresOnceAfterPublishingAndReaping) {
  auto engine = ProcessEngine::start();
  ASSERT_TRUE(engine.has_value()) << engine.error().diagnostic;

  std::string pid_template = (std::filesystem::temp_directory_path() /
                              "libtmux-process-engine-retirement-XXXXXX")
                                 .string();
  const int pid_descriptor = ::mkstemp(pid_template.data());
  ASSERT_GE(pid_descriptor, 0);
  ScopedPathRemoval remove_pid_file{pid_template};
  ASSERT_EQ(::close(pid_descriptor), 0);
  auto retired = std::make_shared<RetirementObservation>();
  ScopedEngineClose close_engine{*engine};
  auto request = shell("printf $$ > \"$1\"; printf retired");
  request.arguments.push_back({"libtmux-retirement-test"});
  request.arguments.push_back({pid_template});
  auto running = (*engine)->submit(
      std::move(request),
      [pid_template, retired, owned = std::make_unique<int>(1)]() mutable {
        if (retired->calls.fetch_add(*owned, std::memory_order_relaxed) != 0) {
          return;
        }
        int child_pid = -1;
        if (FILE* pid_file = std::fopen(pid_template.c_str(), "r")) {
          static_cast<void>(std::fscanf(pid_file, "%d", &child_pid));
          static_cast<void>(std::fclose(pid_file));
        }
        int status_result = 0;
        int status_error = EINVAL;
        if (child_pid <= 0) {
          retired->status_result.store(status_result, std::memory_order_relaxed);
          retired->status_error.store(status_error, std::memory_order_relaxed);
        } else {
          errno = 0;
          status_result = ::waitpid(child_pid, nullptr, WNOHANG);
          status_error = errno;
          retired->status_result.store(status_result, std::memory_order_relaxed);
          retired->status_error.store(status_error, std::memory_order_relaxed);
        }
        retired->ready.store(true, std::memory_order_release);
      });

  auto reply = sync_wait(std::move(running));
  ASSERT_TRUE(reply.has_value()) << reply.error().diagnostic;
  EXPECT_EQ(text(reply->stdout_bytes), "retired");
  const auto retirement_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{1};
  while (!retired->ready.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < retirement_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  ASSERT_TRUE(retired->ready.load(std::memory_order_acquire));
  EXPECT_EQ(retired->status_result.load(std::memory_order_relaxed), -1);
  EXPECT_EQ(retired->status_error.load(std::memory_order_relaxed), ECHILD);
  EXPECT_EQ(retired->calls.load(std::memory_order_relaxed), 1);
  close_engine.close();
  EXPECT_EQ(retired->calls.load(std::memory_order_relaxed), 1);
}

TEST(ProcessEngine, RefusedTransportRetirementFiresOnce) {
  auto engine =
      ProcessEngine::start(libtmux::detail::EngineConfig{.operation_limit = 0U});
  ASSERT_TRUE(engine.has_value()) << engine.error().diagnostic;
  int calls = 0;

  auto refused = (*engine)->submit(
      shell("true"), [owned = std::make_unique<int>(1), &calls] { calls += *owned; });

  auto reply = sync_wait(std::move(refused));
  ASSERT_FALSE(reply.has_value());
  EXPECT_EQ(reply.error().kind, libtmux::FailureKind::overloaded);
  EXPECT_EQ(calls, 1);
  static_cast<void>((*engine)->close());
  EXPECT_EQ(calls, 1);
}

// The whole point: many children, still two threads.
TEST(ProcessEngine, RunsManyWithoutAThreadForEach) {
  auto engine = ProcessEngine::start();
  ASSERT_TRUE(engine.has_value()) << engine.error().diagnostic;

  std::vector<Operation<ProcessReply>> running;
  running.reserve(24);
  for (int index = 0; index < 24; ++index) {
    running.push_back((*engine)->submit(shell("printf " + std::to_string(index))));
  }

  int answered = 0;
  for (int index = 0; index < 24; ++index) {
    auto reply = sync_wait(std::move(running[static_cast<std::size_t>(index)]));
    ASSERT_TRUE(reply.has_value()) << reply.error().diagnostic;
    EXPECT_EQ(text(reply->stdout_bytes), std::to_string(index));
    ++answered;
  }
  EXPECT_EQ(answered, 24);
}

// Accepted work is bounded, and submission past the bound answers at once
// rather than queueing behind a limit the caller cannot see.
TEST(ProcessEngine, RefusesWorkPastItsBoundWithoutWaiting) {
  auto engine =
      ProcessEngine::start(libtmux::detail::EngineConfig{.operation_limit = 2U});
  ASSERT_TRUE(engine.has_value()) << engine.error().diagnostic;

  std::vector<Operation<ProcessReply>> held;
  held.reserve(8);
  for (int index = 0; index < 8; ++index) {
    held.push_back((*engine)->submit(shell("sleep 2")));
  }

  int refused = 0;
  for (auto& one : held) {
    auto reply = sync_wait(std::move(one));
    if (!reply.has_value() && reply.error().kind == libtmux::FailureKind::overloaded) {
      EXPECT_EQ(reply.error().delivery, libtmux::DeliveryStatus::not_started);
      ++refused;
    }
  }
  EXPECT_GT(refused, 0) << "a bound nothing reaches is not a bound";
}

// A child that outlives its deadline is ended by the engine, not waited for.
TEST(ProcessEngine, EndsAChildThatOutlivesItsDeadline) {
  auto engine = ProcessEngine::start();
  ASSERT_TRUE(engine.has_value()) << engine.error().diagnostic;
  auto request = shell("sleep 30");
  request.timeout = std::chrono::milliseconds{200};

  const auto started = std::chrono::steady_clock::now();
  auto reply = sync_wait((*engine)->submit(std::move(request)));
  const auto took = std::chrono::steady_clock::now() - started;

  ASSERT_FALSE(reply.has_value());
  EXPECT_EQ(reply.error().kind, libtmux::FailureKind::timeout);
  EXPECT_LT(took, std::chrono::seconds{10});
}

TEST(ProcessEngine, AnExpiredRequestDoesNotStart) {
  auto engine = ProcessEngine::start();
  ASSERT_TRUE(engine.has_value()) << engine.error().diagnostic;

  std::string marker_template = "/tmp/libtmux-process-engine-expired-XXXXXX";
  const int marker_descriptor = ::mkstemp(marker_template.data());
  ASSERT_GE(marker_descriptor, 0);
  ASSERT_EQ(::close(marker_descriptor), 0);
  ASSERT_TRUE(std::filesystem::remove(marker_template));
  auto request = shell("printf started > " + marker_template);
  request.timeout = std::chrono::milliseconds::zero();

  auto reply = sync_wait((*engine)->submit(std::move(request)));

  ASSERT_FALSE(reply.has_value());
  EXPECT_EQ(reply.error().kind, libtmux::FailureKind::timeout);
  EXPECT_EQ(reply.error().delivery, libtmux::DeliveryStatus::not_started);
  EXPECT_FALSE(std::filesystem::exists(marker_template));
}

// Shutdown ends outstanding work; it does not wait for it. An engine that
// waits is an engine whose teardown takes as long as the slowest tmux command
// anyone happened to have running.
TEST(ProcessEngine, ShutdownEndsOutstandingWorkRatherThanWaitingForIt) {
  auto engine = ProcessEngine::start();
  ASSERT_TRUE(engine.has_value()) << engine.error().diagnostic;
  auto running = (*engine)->submit(shell("sleep 30"));
  std::this_thread::sleep_for(std::chrono::milliseconds{200});

  const auto started = std::chrono::steady_clock::now();
  const auto report = (*engine)->close();
  const auto took = std::chrono::steady_clock::now() - started;

  EXPECT_LT(took, std::chrono::seconds{3});
  EXPECT_TRUE(report.complete);
  // Every accepted operation is answered, rather than left for a caller that
  // would wait for a reply nobody is going to send.
  auto reply = sync_wait(std::move(running));
  EXPECT_FALSE(reply.has_value());
}

// A cancelled call is not a call that ran out of time. Reporting one as the
// other tells a caller their deadline was too short when they withdrew it.
TEST(ProcessEngine, ReportsACancelledCallAsCancelled) {
  auto engine = ProcessEngine::start();
  ASSERT_TRUE(engine.has_value()) << engine.error().diagnostic;
  auto running = (*engine)->submit(shell("sleep 30"));
  std::this_thread::sleep_for(std::chrono::milliseconds{150});

  EXPECT_TRUE(running.request_cancel());
  auto reply = sync_wait(std::move(running));

  ASSERT_FALSE(reply.has_value());
  EXPECT_EQ(reply.error().kind, libtmux::FailureKind::cancelled);
}

// Output that outruns the reader still arrives whole, because the child's end
// of the pipe waits rather than failing.
TEST(ProcessEngine, DeliversOutputLargerThanAPipeBuffer) {
  auto engine = ProcessEngine::start();
  ASSERT_TRUE(engine.has_value()) << engine.error().diagnostic;
  auto request = shell("seq 1 40000");
  request.capture_limit = 1024U * 1024U;

  auto reply = sync_wait((*engine)->submit(std::move(request)));

  ASSERT_TRUE(reply.has_value()) << reply.error().diagnostic;
  ASSERT_TRUE(std::holds_alternative<Exited>(reply->termination));
  EXPECT_EQ(std::get<Exited>(reply->termination).code, 0);
  EXPECT_GT(reply->stdout_bytes.size(), 200000U);
  EXPECT_FALSE(reply->output_truncated);
}

TEST(ProcessEngine, EveryReadableChildMakesOutputProgress) {
  auto engine = ProcessEngine::start();
  ASSERT_TRUE(engine.has_value()) << engine.error().diagnostic;

  std::string marker_template = "/tmp/libtmux-process-engine-fairness-XXXXXX";
  ASSERT_NE(::mkdtemp(marker_template.data()), nullptr);
  constexpr int producer_count = 8;
  const auto gate = marker_template + "/go";
  std::vector<Operation<ProcessReply>> producers;
  producers.reserve(producer_count);
  for (int index = 0; index < producer_count; ++index) {
    auto producer =
        shell("printf ready; printf ready > " + marker_template + "/" +
              std::to_string(index) + "; while [ ! -e " + gate +
              " ]; do sleep 0.05; done; printf -- -" + std::to_string(index));
    producer.timeout = std::chrono::seconds{2};
    producers.push_back((*engine)->submit(std::move(producer)));
  }

  const auto marker_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{2};
  const auto all_ready = [&] {
    for (int index = 0; index < producer_count; ++index) {
      if (!std::filesystem::exists(marker_template + "/" + std::to_string(index))) {
        return false;
      }
    }
    return true;
  };
  while (!all_ready() && std::chrono::steady_clock::now() < marker_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  const bool producers_ready = all_ready();
  ASSERT_TRUE(producers_ready) << "a producer did not make its prefix readable";
  ASSERT_TRUE(std::filesystem::create_directory(gate));

  for (int index = 0; index < producer_count; ++index) {
    auto reply = sync_wait(std::move(producers[static_cast<std::size_t>(index)]));
    ASSERT_TRUE(reply.has_value()) << reply.error().diagnostic;
    EXPECT_EQ(text(reply->stdout_bytes), "ready-" + std::to_string(index));
    EXPECT_FALSE(reply->output_truncated);
  }
  EXPECT_EQ(std::filesystem::remove_all(marker_template),
            static_cast<std::uintmax_t>(producer_count + 2));
}

// A refusal never held a slot, so handing one back on the way out gives away
// admission nobody took. Enough refusals and the bound stops bounding.
TEST(ProcessEngine, ARefusalDoesNotReturnASlotItNeverHeld) {
  auto engine =
      ProcessEngine::start(libtmux::detail::EngineConfig{.operation_limit = 1U});
  ASSERT_TRUE(engine.has_value()) << engine.error().diagnostic;

  auto occupying = (*engine)->submit(shell("sleep 5"));
  auto refused = sync_wait((*engine)->submit(shell("true")));
  ASSERT_FALSE(refused.has_value());
  ASSERT_EQ(refused.error().kind, libtmux::FailureKind::overloaded);

  auto again = sync_wait((*engine)->submit(shell("true")));

  ASSERT_FALSE(again.has_value()) << "the one slot is still occupied";
  EXPECT_EQ(again.error().kind, libtmux::FailureKind::overloaded);
}

// The engine is gone once the last caller lets go. Threads that held a
// reference of their own kept it alive for the life of the process, so its
// shutdown only ever ran in a test that called it by hand.
TEST(ProcessEngine, EndsWhenTheLastHolderLetsGo) {
  std::weak_ptr<ProcessEngine> watched;
  {
    auto engine = ProcessEngine::start();
    ASSERT_TRUE(engine.has_value()) << engine.error().diagnostic;
    watched = *engine;
    auto reply = sync_wait((*engine)->submit(shell("true")));
    ASSERT_TRUE(reply.has_value()) << reply.error().diagnostic;
  }
  EXPECT_TRUE(watched.expired()) << "the engine's own threads still hold it";
}

// Shutdown does not start what it is about to end. A queue drained into
// process creation at close time runs real commands, with whatever they do to
// the world, only to report every one of them as cancelled.
TEST(ProcessEngine, ShutdownDoesNotStartWorkItIsAboutToEnd) {
  auto engine = ProcessEngine::start();
  ASSERT_TRUE(engine.has_value()) << engine.error().diagnostic;

  std::vector<Operation<ProcessReply>> queued;
  queued.reserve(64);
  for (int index = 0; index < 64; ++index) {
    queued.push_back((*engine)->submit(shell("sleep 30")));
  }
  static_cast<void>((*engine)->close());

  int never_started = 0;
  for (auto& one : queued) {
    auto reply = sync_wait(std::move(one));
    ASSERT_FALSE(reply.has_value());
    if (reply.error().delivery == libtmux::DeliveryStatus::not_started) {
      ++never_started;
    }
  }
  EXPECT_GT(never_started, 0) << "every queued command was started, then ended";
}

TEST(ProcessEngine, ShutdownWaitsForALaunchAlreadyInFlight) {
  LaunchShutdownBarrier barrier;
  auto engine = ProcessEngine::start(
      {.launch_observer = [&barrier](const ProcessRequest&) { barrier.hold_launch(); },
       .reactor_observer =
           [&barrier](libtmux::detail::EngineReactorEvent event) {
             barrier.observe(event);
           }});
  ASSERT_TRUE(engine.has_value()) << engine.error().diagnostic;

  std::string executable_template =
      (std::filesystem::temp_directory_path() / "libtmux-process-engine-launch-XXXXXX")
          .string();
  const int executable_descriptor = ::mkstemp(executable_template.data());
  ASSERT_GE(executable_descriptor, 0);
  ScopedPathRemoval remove_executable{executable_template};
  ASSERT_EQ(::close(executable_descriptor), 0);
  ASSERT_TRUE(std::filesystem::remove(executable_template));
  ASSERT_NO_THROW(std::filesystem::create_symlink(executable_template + "-missing",
                                                  executable_template));
  auto request = shell("sleep 30");
  request.executable = executable_template;
  auto running = (*engine)->submit(std::move(request));

  const bool launch_withheld = barrier.wait_until_launch_is_withheld();
  if (!launch_withheld) {
    barrier.release();
    FAIL() << "the launch lane did not take the accepted request";
    return;
  }
  std::optional<libtmux::detail::EngineShutdown> shutdown;
  std::thread closer{[&] { shutdown = (*engine)->close(); }};
  const auto first_event = barrier.wait_for_reactor_event();
  std::error_code executable_error;
  bool executable_enabled = false;
  if (first_event &&
      *first_event == libtmux::detail::EngineReactorEvent::waiting_for_launch &&
      std::filesystem::remove(executable_template, executable_error)) {
    std::filesystem::create_symlink("/bin/sh", executable_template, executable_error);
    executable_enabled = !executable_error;
  }
  barrier.release();
  closer.join();
  auto reply = sync_wait(std::move(running));

  ASSERT_TRUE(first_event.has_value())
      << "the reactor did not inspect shutdown while launch was withheld";
  EXPECT_EQ(*first_event, libtmux::detail::EngineReactorEvent::waiting_for_launch);
  EXPECT_TRUE(executable_enabled)
      << (executable_error ? executable_error.message()
                           : "the reactor exited before enabling the executable");
  ASSERT_TRUE(shutdown.has_value());
  EXPECT_EQ(shutdown->operations_published, 1U);
  EXPECT_EQ(shutdown->children_reaped, 1U);
  EXPECT_TRUE(shutdown->complete);
  ASSERT_FALSE(reply.has_value());
  EXPECT_EQ(reply.error().kind, libtmux::FailureKind::cancelled);
  EXPECT_EQ(reply.error().delivery, libtmux::DeliveryStatus::indeterminate);
  EXPECT_EQ(barrier.event_count(), 2U);
}

} // namespace
