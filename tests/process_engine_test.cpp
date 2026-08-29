// The asynchronous engine against real processes: two threads, whatever the
// load, and an answer only once the child has exited and its output has ended.
#include "process_engine.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

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

} // namespace
