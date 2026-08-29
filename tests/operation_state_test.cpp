#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "completion_queue.hpp"
#include "move_only_function.hpp"

namespace {

using libtmux::detail::MoveOnlyFunction;
using namespace std::chrono_literals;
using libtmux::detail::CompletionQueue;
using libtmux::detail::CompletionToken;

static_assert(!std::is_copy_constructible_v<MoveOnlyFunction<void()>>);
static_assert(std::is_nothrow_move_constructible_v<MoveOnlyFunction<void()>>);
static_assert(std::is_trivially_copyable_v<CompletionToken>);
static_assert(sizeof(CompletionToken) == sizeof(std::uint64_t));

TEST(MoveOnlyFunction, InvokesAndReleasesAMoveOnlyCapture) {
  auto value = std::make_unique<int>(41);
  MoveOnlyFunction<int(int)> function{
      [owned = std::move(value)](int addend) { return *owned + addend; }};

  ASSERT_TRUE(function);
  EXPECT_EQ(function(1), 42);

  MoveOnlyFunction<int(int)> moved{std::move(function)};
  EXPECT_FALSE(function);
  EXPECT_EQ(moved(2), 43);
  moved = {};
  EXPECT_FALSE(moved);
}

TEST(CompletionQueue, EnqueueIsIdempotentAndDispatchesOnce) {
  CompletionQueue queue;
  const CompletionToken token = queue.next_token();
  int calls = 0;
  ASSERT_TRUE(queue.register_record(token, [&] { ++calls; }));

  const auto mailbox = queue.mailbox();
  EXPECT_TRUE(mailbox.enqueue(token));
  EXPECT_FALSE(mailbox.enqueue(token));
  EXPECT_EQ(queue.run_ready(), 1U);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(queue.run_ready(), 0U);
}

TEST(CompletionQueue, ATokenCanBeEnqueuedAfterItsRecordAppears) {
  CompletionQueue queue;
  const CompletionToken token = queue.next_token();
  const auto mailbox = queue.mailbox();
  EXPECT_FALSE(mailbox.enqueue(token));

  int calls = 0;
  ASSERT_TRUE(queue.register_record(token, [&] { ++calls; }));
  EXPECT_TRUE(mailbox.enqueue(token));
  EXPECT_TRUE(queue.run_one());
  EXPECT_EQ(calls, 1);
}

struct CaptureThread final {
  explicit CaptureThread(std::promise<std::thread::id>& promise) : promise{promise} {}
  ~CaptureThread() { promise.set_value(std::this_thread::get_id()); }

  std::promise<std::thread::id>& promise;
};

TEST(CompletionQueue, CloseDestroysCapturesOnTheClosingThread) {
  auto queue = std::make_unique<CompletionQueue>();
  std::promise<std::thread::id> destroyed;
  auto destroyed_on = destroyed.get_future();
  auto capture = std::make_shared<CaptureThread>(destroyed);
  const CompletionToken token = queue->next_token();
  ASSERT_TRUE(queue->register_record(token, [capture] {}));
  capture.reset();

  std::thread::id closing_thread;
  std::thread closer{[&] {
    closing_thread = std::this_thread::get_id();
    queue.reset();
  }};
  closer.join();

  EXPECT_EQ(destroyed_on.get(), closing_thread);
}

TEST(CompletionQueue, DetachDestroysCapturesOnTheDetachingThread) {
  CompletionQueue queue;
  std::promise<std::thread::id> destroyed;
  auto destroyed_on = destroyed.get_future();
  auto capture = std::make_shared<CaptureThread>(destroyed);
  const CompletionToken token = queue.next_token();
  ASSERT_TRUE(queue.register_record(token, [capture] {}));
  capture.reset();

  std::thread::id detaching_thread;
  std::thread detacher{[&] {
    detaching_thread = std::this_thread::get_id();
    queue.detach(token);
  }};
  detacher.join();

  EXPECT_EQ(destroyed_on.get(), detaching_thread);
}

TEST(CompletionQueue, DispatchDestroysCapturesOnTheDispatchingThread) {
  CompletionQueue queue;
  std::promise<std::thread::id> destroyed;
  auto destroyed_on = destroyed.get_future();
  auto capture = std::make_shared<CaptureThread>(destroyed);
  const CompletionToken token = queue.next_token();
  ASSERT_TRUE(queue.register_record(token, [capture] {}));
  capture.reset();
  ASSERT_TRUE(queue.mailbox().enqueue(token));

  std::thread::id dispatching_thread;
  std::thread dispatcher{[&] {
    dispatching_thread = std::this_thread::get_id();
    EXPECT_TRUE(queue.run_one());
  }};
  dispatcher.join();

  EXPECT_EQ(destroyed_on.get(), dispatching_thread);
}

TEST(CompletionQueue, ADispatchClaimWinsAgainstClose) {
  CompletionQueue queue;
  const CompletionToken token = queue.next_token();
  std::promise<void> entered;
  auto callback_entered = entered.get_future();
  std::promise<void> release;
  auto released = release.get_future().share();
  std::atomic<int> calls{0};
  ASSERT_TRUE(queue.register_record(token, [&] {
    calls.fetch_add(1, std::memory_order_relaxed);
    entered.set_value();
    released.wait();
  }));
  ASSERT_TRUE(queue.mailbox().enqueue(token));

  std::thread dispatcher{[&] { EXPECT_TRUE(queue.run_one()); }};
  callback_entered.get();
  queue.close();
  release.set_value();
  dispatcher.join();

  EXPECT_EQ(calls.load(std::memory_order_relaxed), 1);
  EXPECT_FALSE(queue.mailbox().enqueue(token));
}

TEST(CompletionQueue, CallbackExceptionsLeaveLaterRecordsReady) {
  CompletionQueue queue;
  const auto first = queue.next_token();
  const auto second = queue.next_token();
  int calls = 0;
  ASSERT_TRUE(queue.register_record(first, [&] {
    ++calls;
    throw std::runtime_error{"caller failure"};
  }));
  ASSERT_TRUE(queue.register_record(second, [&] { ++calls; }));
  ASSERT_TRUE(queue.mailbox().enqueue(first));
  ASSERT_TRUE(queue.mailbox().enqueue(second));

  EXPECT_THROW(static_cast<void>(queue.run_ready()), std::runtime_error);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(queue.run_ready(), 1U);
  EXPECT_EQ(calls, 2);
}

} // namespace
