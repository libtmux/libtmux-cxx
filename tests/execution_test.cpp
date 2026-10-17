#include "libtmux/execution.hpp"
#include "libtmux/testing/scoped_server.hpp"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <exec/static_thread_pool.hpp>
#include <gtest/gtest.h>
#include <mutex>
#include <optional>
#include <utility>

namespace {

class Execution : public testing::Test {
protected:
  void SetUp() override {
    auto started = libtmux::test::ScopedTmuxServer::start(
        {.session_name = "execution", .socket_namespace = {"execution-test"}});
    ASSERT_TRUE(started);
    fixture.emplace(std::move(*started));
    auto connected = libtmux::Server::at_socket_path(fixture->socket_path().string());
    ASSERT_TRUE(connected);
    server.emplace(std::move(*connected));
    auto started_runtime = libtmux::CommandRuntime::start({.capacity = 8});
    ASSERT_TRUE(started_runtime);
    runtime.emplace(std::move(*started_runtime));
  }

  std::optional<libtmux::test::ScopedTmuxServer> fixture;
  std::optional<libtmux::Server> server;
  std::optional<libtmux::CommandRuntime> runtime;
};

TEST_F(Execution, AdmissionIsLazyAndCommandsCompose) {
  exec::static_thread_pool pool{2};
  auto first = libtmux::command_sender(pool.get_scheduler(), *server, *runtime,
                                       {"display-message", "-p", "#{session_name}"});
  auto second = libtmux::command_sender(pool.get_scheduler(), *server, *runtime,
                                        {"display-message", "-p", "#{session_name}"});
  EXPECT_EQ(runtime->snapshot().accepted, 0U);
  auto result =
      stdexec::sync_wait(stdexec::when_all(std::move(first), std::move(second)));
  ASSERT_TRUE(result);
  ASSERT_TRUE(std::get<0>(*result));
  ASSERT_TRUE(std::get<1>(*result));
  EXPECT_EQ(*std::get<0>(*result), "execution\n");
  EXPECT_EQ(*std::get<1>(*result), "execution\n");
  const auto report = runtime->close();
  EXPECT_TRUE(report.transports_stopped);
  EXPECT_EQ(report.pending_results, 0U);
}

TEST_F(Execution, TwoWorkersCanSignalAWaitingCommand) {
  exec::static_thread_pool pool{2};
  auto wait = libtmux::command_sender(pool.get_scheduler(), *server, *runtime,
                                      {"wait-for", "execution-pair"});
  auto signal = libtmux::command_sender(pool.get_scheduler(), *server, *runtime,
                                        {"wait-for", "-S", "execution-pair"});
  auto result =
      stdexec::sync_wait(stdexec::when_all(std::move(wait), std::move(signal)));
  ASSERT_TRUE(result);
  ASSERT_TRUE(std::get<0>(*result));
  ASSERT_TRUE(std::get<1>(*result));
  EXPECT_TRUE(std::get<0>(*result)->empty());
  EXPECT_TRUE(std::get<1>(*result)->empty());
}

TEST_F(Execution, TheSenderOwnsItsServerHandle) {
  exec::static_thread_pool pool{1};
  auto sender = [&] {
    auto local_server = *server;
    return libtmux::command_sender(pool.get_scheduler(), local_server, *runtime,
                                   {"display-message", "-p", "#{session_name}"});
  }();
  auto result = stdexec::sync_wait(std::move(sender));
  ASSERT_TRUE(result);
  ASSERT_TRUE(std::get<0>(*result));
  EXPECT_EQ(*std::get<0>(*result), "execution\n");
}

TEST_F(Execution, RefusalKeepsTheCommandFailureAndDelivery) {
  exec::static_thread_pool pool{1};
  auto result = stdexec::sync_wait(
      libtmux::command_sender(pool.get_scheduler(), *server, *runtime, {}));
  ASSERT_TRUE(result);
  const auto& answer = std::get<0>(*result);
  ASSERT_FALSE(answer);
  EXPECT_EQ(answer.error().kind, libtmux::FailureKind::validation);
  EXPECT_EQ(answer.error().delivery, libtmux::DeliveryStatus::not_started);
}

TEST_F(Execution, TmuxFailureRetainsItsDiagnostic) {
  exec::static_thread_pool pool{1};
  auto result = stdexec::sync_wait(libtmux::command_sender(
      pool.get_scheduler(), *server, *runtime, {"execution-no-such-command"}));
  ASSERT_TRUE(result);
  const auto& answer = std::get<0>(*result);
  ASSERT_FALSE(answer);
  EXPECT_NE(answer.error().kind, libtmux::FailureKind::cancelled);
  EXPECT_FALSE(answer.error().diagnostic.empty());
}

struct Completion {
  std::mutex mutex;
  std::condition_variable changed;
  std::optional<libtmux::CommandFailure> failure;
  bool done{};
  bool unexpected{};
};

struct Receiver {
  using receiver_concept = stdexec::receiver_tag;
  Completion* completion;

  auto get_env() const noexcept { return stdexec::env{}; }
  void set_value(
      libtmux::expected<std::string, libtmux::CommandFailure> answer) && noexcept {
    finish(answer ? std::nullopt : std::optional{std::move(answer.error())});
  }
  void set_error(libtmux::CommandFailure failure) && noexcept {
    finish(std::move(failure));
  }
  void set_error(std::exception_ptr) && noexcept { finish(std::nullopt); }
  void set_stopped() && noexcept { finish(std::nullopt); }

private:
  void finish(std::optional<libtmux::CommandFailure> failure) noexcept {
    {
      std::lock_guard lock{completion->mutex};
      completion->failure = std::move(failure);
      completion->unexpected = !completion->failure;
      completion->done = true;
    }
    completion->changed.notify_one();
  }
};

TEST_F(Execution, ExplicitStopBeforeAndAfterAdmissionKeepsCancellationInformation) {
  exec::static_thread_pool pool{1};
  for (const bool before_start : {true, false}) {
    stdexec::inplace_stop_source stop;
    Completion completion;
    if (before_start) {
      stop.request_stop();
    }
    auto operation = stdexec::connect(
        libtmux::command_sender(pool.get_scheduler(), *server, *runtime,
                                {"wait-for", "-S", "execution-admitted", ";",
                                 "wait-for", "execution-cancel"},
                                stop.get_token()),
        Receiver{&completion});
    stdexec::start(operation);
    if (!before_start) {
      EXPECT_TRUE(
          server->wait_for("execution-admitted", std::chrono::milliseconds{200}));
      EXPECT_EQ(runtime->snapshot().accepted, 1U);
      stop.request_stop();
    }
    std::unique_lock lock{completion.mutex};
    const bool completed = completion.changed.wait_for(
        lock, std::chrono::milliseconds{200}, [&] { return completion.done; });
    if (!completed) {
      lock.unlock();
      EXPECT_TRUE(server->signal("execution-cancel"));
      lock.lock();
      EXPECT_TRUE(completion.changed.wait_for(lock, std::chrono::milliseconds{200},
                                              [&] { return completion.done; }));
    }
    EXPECT_TRUE(completed);
    ASSERT_TRUE(completion.done);
    ASSERT_FALSE(completion.unexpected);
    ASSERT_TRUE(completion.failure);
    EXPECT_EQ(completion.failure->kind, libtmux::FailureKind::cancelled);
    if (before_start) {
      EXPECT_EQ(completion.failure->delivery, libtmux::DeliveryStatus::not_started);
    } else {
      EXPECT_EQ(completion.failure->delivery, libtmux::DeliveryStatus::indeterminate);
    }
  }
}

} // namespace
