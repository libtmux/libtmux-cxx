// The asynchronous engine against real processes: two threads, whatever the
// load, and an answer only once the child has exited and its output has ended.
#include "process_engine.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

// Work being launched is in neither queue: the launch lane has taken it and
// the reactor has not been handed it yet. A reactor that reads that gap as
// nothing left returns, and the child it was about to own is never waited for
// by anyone. Repeated because the window is the width of one process
// creation.
TEST(ProcessEngine, ShutdownWaitsForALaunchAlreadyInFlight) {
  for (int attempt = 0; attempt < 32; ++attempt) {
    Operation<ProcessReply> running;
    {
      auto engine = ProcessEngine::start();
      ASSERT_TRUE(engine.has_value()) << engine.error().diagnostic;
      running = (*engine)->submit(shell("sleep 5"));
      static_cast<void>((*engine)->close());
    }
    auto reply = sync_wait(std::move(running));
    EXPECT_FALSE(reply.has_value());
  }
}

} // namespace
