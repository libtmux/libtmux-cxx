#include "command_engine.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <semaphore>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using libtmux::CommandFailure;
using libtmux::CommandRequest;
using libtmux::DeliveryStatus;
using libtmux::expected;
using libtmux::FailureKind;
using libtmux::unexpected;
using libtmux::detail::CommandEngine;
using libtmux::detail::CommandEngineConfig;
using libtmux::detail::ProcessTransportEntry;
using libtmux::detail::sync_wait;

class OrderedEntryHarness final {
public:
  void dequeued(const CommandRequest& command) {
    if (id(command) == "1") {
      first_dequeued_.release();
      release_first_dequeue_.acquire();
      return;
    }
    second_dequeued_.release();
  }

  expected<std::string, CommandFailure> run(const CommandRequest& command,
                                            std::optional<std::chrono::milliseconds>,
                                            std::optional<std::size_t>,
                                            const std::function<bool()>&,
                                            ProcessTransportEntry entry) {
    const std::string task = id(command);
    {
      std::lock_guard lock{entries_mutex_};
      entries_.push_back(task);
    }
    entry.entered();
    if (task == "1") {
      first_entered_.release();
      finish_first_.acquire();
      first_finished_.release();
    } else {
      second_entered_.release();
    }
    return task;
  }

  [[nodiscard]] bool wait_for_first_dequeue() {
    return first_dequeued_.try_acquire_for(2s);
  }
  [[nodiscard]] bool wait_for_second_dequeue() {
    return second_dequeued_.try_acquire_for(2s);
  }
  [[nodiscard]] bool second_entered_while_first_paused() {
    if (!second_entered_.try_acquire_for(100ms)) {
      return false;
    }
    second_entered_.release();
    return true;
  }
  void release_first_dequeue() { release_first_dequeue_.release(); }
  [[nodiscard]] bool wait_for_first_entry() {
    return first_entered_.try_acquire_for(2s);
  }
  [[nodiscard]] bool wait_for_second_entry() {
    return second_entered_.try_acquire_for(2s);
  }
  [[nodiscard]] bool first_finished() { return first_finished_.try_acquire(); }
  void finish_first() { finish_first_.release(); }

  [[nodiscard]] std::vector<std::string> entries() const {
    std::lock_guard lock{entries_mutex_};
    return entries_;
  }

private:
  [[nodiscard]] static std::string id(const CommandRequest& command) {
    return command.arguments().front().value();
  }

  std::binary_semaphore first_dequeued_{0};
  std::binary_semaphore second_dequeued_{0};
  std::binary_semaphore release_first_dequeue_{0};
  std::binary_semaphore first_entered_{0};
  std::binary_semaphore second_entered_{0};
  std::binary_semaphore finish_first_{0};
  std::binary_semaphore first_finished_{0};
  mutable std::mutex entries_mutex_;
  std::vector<std::string> entries_;
};

TEST(CommandEngineOrder, LaunchesInAdmissionOrderThenRunsConcurrently) {
  auto harness = std::make_shared<OrderedEntryHarness>();
  auto engine = CommandEngine::start(CommandEngineConfig{
      .operation_limit = 2U,
      .worker_count = 2U,
      .dequeue_observer =
          [harness](const CommandRequest& command) { harness->dequeued(command); },
      .runner =
          [harness](const CommandRequest& command,
                    std::optional<std::chrono::milliseconds> timeout,
                    std::optional<std::size_t> output_limit,
                    const std::function<bool()>& cancelled,
                    ProcessTransportEntry entry) {
            return harness->run(command, timeout, output_limit, cancelled, entry);
          },
  });
  ASSERT_TRUE(engine.has_value()) << engine.error().diagnostic;

  auto first = (*engine)->submit({}, {"1"}, std::nullopt, std::nullopt);
  ASSERT_TRUE(harness->wait_for_first_dequeue());
  auto second = (*engine)->submit({}, {"2"}, std::nullopt, std::nullopt);
  ASSERT_TRUE(harness->wait_for_second_dequeue());

  EXPECT_FALSE(harness->second_entered_while_first_paused());
  harness->release_first_dequeue();
  ASSERT_TRUE(harness->wait_for_first_entry());
  ASSERT_TRUE(harness->wait_for_second_entry());

  auto second_answer = sync_wait(std::move(second));
  ASSERT_TRUE(second_answer.has_value()) << second_answer.error().diagnostic;
  EXPECT_EQ(*second_answer, "2");
  EXPECT_FALSE(harness->first_finished());

  harness->finish_first();
  auto first_answer = sync_wait(std::move(first));
  ASSERT_TRUE(first_answer.has_value()) << first_answer.error().diagnostic;
  EXPECT_EQ(*first_answer, "1");
  EXPECT_EQ(harness->entries(), (std::vector<std::string>{"1", "2"}));
  (*engine)->close();
}

class SkippedEntryHarness final {
public:
  expected<std::string, CommandFailure> run(const CommandRequest& command,
                                            std::optional<std::chrono::milliseconds>,
                                            std::optional<std::size_t>,
                                            const std::function<bool()>&,
                                            ProcessTransportEntry entry) {
    const std::string task = command.arguments().front().value();
    if (task == "fail") {
      return unexpected(CommandFailure{
          .kind = FailureKind::spawn,
          .delivery = DeliveryStatus::not_started,
          .exit_code = 0,
          .diagnostic = "controlled pre-launch failure",
      });
    }
    {
      std::lock_guard lock{entries_mutex_};
      entries_.push_back(task);
    }
    entry.entered();
    return task;
  }

  [[nodiscard]] std::vector<std::string> entries() const {
    std::lock_guard lock{entries_mutex_};
    return entries_;
  }

private:
  mutable std::mutex entries_mutex_;
  std::vector<std::string> entries_;
};

TEST(CommandEngineOrder, SkipsPreLaunchFailureWithoutClaimingEntry) {
  auto harness = std::make_shared<SkippedEntryHarness>();
  auto engine = CommandEngine::start(CommandEngineConfig{
      .operation_limit = 2U,
      .worker_count = 2U,
      .runner =
          [harness](const CommandRequest& command,
                    std::optional<std::chrono::milliseconds> timeout,
                    std::optional<std::size_t> output_limit,
                    const std::function<bool()>& cancelled,
                    ProcessTransportEntry entry) {
            return harness->run(command, timeout, output_limit, cancelled, entry);
          },
  });
  ASSERT_TRUE(engine.has_value()) << engine.error().diagnostic;

  auto failed = (*engine)->submit({}, {"fail"}, std::nullopt, std::nullopt);
  auto next = (*engine)->submit({}, {"next"}, std::nullopt, std::nullopt);

  auto failed_answer = sync_wait(std::move(failed));
  ASSERT_FALSE(failed_answer.has_value());
  EXPECT_EQ(failed_answer.error().kind, FailureKind::spawn);
  EXPECT_EQ(failed_answer.error().delivery, DeliveryStatus::not_started);
  auto next_answer = sync_wait(std::move(next));
  ASSERT_TRUE(next_answer.has_value()) << next_answer.error().diagnostic;
  EXPECT_EQ(*next_answer, "next");
  EXPECT_EQ(harness->entries(), (std::vector<std::string>{"next"}));
  (*engine)->close();
}

TEST(CommandEngineOrder, SkipsCancelledAndExpiredWorkBeforeEntry) {
  std::binary_semaphore first_dequeued{0};
  std::binary_semaphore release_first{0};
  auto harness = std::make_shared<SkippedEntryHarness>();
  auto engine = CommandEngine::start(CommandEngineConfig{
      .operation_limit = 3U,
      .worker_count = 2U,
      .dequeue_observer =
          [&first_dequeued, &release_first](const CommandRequest& command) {
            if (command.arguments().front().value() == "cancel") {
              first_dequeued.release();
              release_first.acquire();
            }
          },
      .runner =
          [harness](const CommandRequest& command,
                    std::optional<std::chrono::milliseconds> timeout,
                    std::optional<std::size_t> output_limit,
                    const std::function<bool()>& cancelled,
                    ProcessTransportEntry entry) {
            return harness->run(command, timeout, output_limit, cancelled, entry);
          },
  });
  ASSERT_TRUE(engine.has_value()) << engine.error().diagnostic;

  auto cancelled = (*engine)->submit({}, {"cancel"}, std::nullopt, std::nullopt);
  ASSERT_TRUE(first_dequeued.try_acquire_for(2s));
  auto expired = (*engine)->submit({}, {"expire"}, 0ms, std::nullopt);
  auto next = (*engine)->submit({}, {"next"}, std::nullopt, std::nullopt);
  ASSERT_TRUE(cancelled.request_cancel());
  release_first.release();

  auto cancelled_answer = sync_wait(std::move(cancelled));
  ASSERT_FALSE(cancelled_answer.has_value());
  EXPECT_EQ(cancelled_answer.error().kind, FailureKind::cancelled);
  EXPECT_EQ(cancelled_answer.error().delivery, DeliveryStatus::not_started);
  auto expired_answer = sync_wait(std::move(expired));
  ASSERT_FALSE(expired_answer.has_value());
  EXPECT_EQ(expired_answer.error().kind, FailureKind::timeout);
  EXPECT_EQ(expired_answer.error().delivery, DeliveryStatus::not_started);
  auto next_answer = sync_wait(std::move(next));
  ASSERT_TRUE(next_answer.has_value()) << next_answer.error().diagnostic;
  EXPECT_EQ(*next_answer, "next");
  EXPECT_EQ(harness->entries(), (std::vector<std::string>{"next"}));
  (*engine)->close();
}

TEST(CommandEngineOrder, StopWakesAWorkerWaitingForItsTurn) {
  std::binary_semaphore first_dequeued{0};
  std::binary_semaphore second_dequeued{0};
  std::binary_semaphore release_first{0};
  auto engine = CommandEngine::start(CommandEngineConfig{
      .operation_limit = 2U,
      .worker_count = 2U,
      .dequeue_observer =
          [&first_dequeued, &second_dequeued,
           &release_first](const CommandRequest& command) {
            if (command.arguments().front().value() == "1") {
              first_dequeued.release();
              release_first.acquire();
            } else {
              second_dequeued.release();
            }
          },
      .runner =
          [](const CommandRequest&, std::optional<std::chrono::milliseconds>,
             std::optional<std::size_t>, const std::function<bool()>&,
             ProcessTransportEntry entry) -> expected<std::string, CommandFailure> {
        entry.entered();
        return "unexpected launch";
      },
  });
  ASSERT_TRUE(engine.has_value()) << engine.error().diagnostic;
  auto first = (*engine)->submit({}, {"1"}, std::nullopt, std::nullopt);
  ASSERT_TRUE(first_dequeued.try_acquire_for(2s));
  auto second = (*engine)->submit({}, {"2"}, std::nullopt, std::nullopt);
  ASSERT_TRUE(second_dequeued.try_acquire_for(2s));

  auto closing =
      std::async(std::launch::async, [engine = *engine] { engine->close(); });
  auto second_answer = sync_wait(std::move(second));
  ASSERT_FALSE(second_answer.has_value());
  EXPECT_EQ(second_answer.error().kind, FailureKind::cancelled);
  EXPECT_EQ(second_answer.error().delivery, DeliveryStatus::not_started);

  release_first.release();
  auto first_answer = sync_wait(std::move(first));
  ASSERT_FALSE(first_answer.has_value());
  EXPECT_EQ(first_answer.error().kind, FailureKind::cancelled);
  EXPECT_EQ(first_answer.error().delivery, DeliveryStatus::not_started);
  EXPECT_EQ(closing.wait_for(2s), std::future_status::ready);
}

} // namespace
