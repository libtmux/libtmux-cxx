#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "completion_queue.hpp"
#include "move_only_function.hpp"
#include "operation_state.hpp"

namespace {

using libtmux::detail::MoveOnlyFunction;
using namespace std::chrono_literals;
using libtmux::DeliveryStatus;
using libtmux::expected;
using libtmux::FailureKind;
using libtmux::unexpected;
using libtmux::detail::CompletionQueue;
using libtmux::detail::CompletionToken;
using libtmux::detail::make_operation;
using libtmux::detail::Operation;
using libtmux::detail::OperationHooks;
using libtmux::detail::OperationResult;
using libtmux::detail::sync_wait;

static_assert(!std::is_copy_constructible_v<MoveOnlyFunction<void()>>);
static_assert(std::is_nothrow_move_constructible_v<MoveOnlyFunction<void()>>);
static_assert(std::is_trivially_copyable_v<CompletionToken>);
static_assert(sizeof(CompletionToken) == sizeof(std::uint64_t));
static_assert(!std::is_copy_constructible_v<Operation<int>>);
static_assert(std::is_nothrow_move_constructible_v<Operation<int>>);
static_assert(!std::is_copy_constructible_v<libtmux::detail::OperationSource<int>>);
static_assert(
    std::is_nothrow_move_constructible_v<libtmux::detail::OperationSource<int>>);

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

class RecordingHooks final : public OperationHooks {
public:
  void wake_reactor() noexcept override {
    wakes.fetch_add(1, std::memory_order_relaxed);
  }

  void release_admission() noexcept override {
    releases.fetch_add(1, std::memory_order_relaxed);
  }

  std::atomic<int> wakes{0};
  std::atomic<int> releases{0};
};

TEST(OperationState, CancellationIsOnlyARequestUntilTheSourcePublishes) {
  auto hooks = std::make_shared<RecordingHooks>();
  auto started = make_operation<int>(hooks);

  EXPECT_TRUE(started.operation.request_cancel());
  EXPECT_FALSE(started.operation.request_cancel());
  EXPECT_EQ(hooks->wakes.load(std::memory_order_relaxed), 1);
  EXPECT_FALSE(started.source.outcome_published());

  ASSERT_TRUE(started.source.publish(
      unexpected(libtmux::CommandFailure{.kind = FailureKind::cancelled,
                                         .delivery = DeliveryStatus::not_started,
                                         .diagnostic = {}})));
  started.source.retire();
  EXPECT_EQ(hooks->releases.load(std::memory_order_relaxed), 0);
  const auto result = sync_wait(std::move(started.operation));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().kind, FailureKind::cancelled);
  EXPECT_EQ(hooks->releases.load(std::memory_order_relaxed), 1);
}

TEST(OperationState, AReplyCanBeatARequestedCancellation) {
  auto hooks = std::make_shared<RecordingHooks>();
  auto started = make_operation<int>(hooks);
  ASSERT_TRUE(started.operation.request_cancel());

  EXPECT_TRUE(started.source.publish(OperationResult<int>{43}));
  EXPECT_FALSE(started.source.publish(
      unexpected(libtmux::CommandFailure{.kind = FailureKind::cancelled,
                                         .delivery = DeliveryStatus::not_started,
                                         .diagnostic = {}})));
  started.source.retire();

  const auto result = sync_wait(std::move(started.operation));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 43);
}

TEST(OperationState, SyncWaitBlocksUntilTheSourcePublishes) {
  auto hooks = std::make_shared<RecordingHooks>();
  auto started = make_operation<int>(hooks);
  std::promise<void> entered;
  auto waiter_entered = entered.get_future();
  std::promise<OperationResult<int>> answer;
  auto waited = answer.get_future();
  std::thread waiter{
      [operation = std::move(started.operation), &entered, &answer]() mutable {
        entered.set_value();
        answer.set_value(sync_wait(std::move(operation)));
      }};

  waiter_entered.get();
  EXPECT_EQ(waited.wait_for(50ms), std::future_status::timeout);
  ASSERT_TRUE(started.source.publish(OperationResult<int>{45}));
  const auto result = waited.get();
  waiter.join();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 45);
  EXPECT_EQ(hooks->releases.load(std::memory_order_relaxed), 0);
  started.source.retire();
  EXPECT_EQ(hooks->releases.load(std::memory_order_relaxed), 1);
}

TEST(OperationState, VoidUsesTheSameOwningResultPath) {
  auto hooks = std::make_shared<RecordingHooks>();
  auto started = make_operation<void>(hooks);
  ASSERT_TRUE(started.source.publish(OperationResult<void>{}));
  started.source.retire();

  const auto result = sync_wait(std::move(started.operation));
  EXPECT_TRUE(result.has_value());
  EXPECT_EQ(hooks->releases.load(std::memory_order_relaxed), 1);
}

TEST(OperationState, ConcurrentPublicationChoosesOneImmutableOutcome) {
  auto hooks = std::make_shared<RecordingHooks>();
  auto started = make_operation<int>(hooks);
  std::barrier start{3};
  std::atomic<int> accepted{0};

  std::thread first{[&] {
    start.arrive_and_wait();
    accepted.fetch_add(started.source.publish(OperationResult<int>{1}),
                       std::memory_order_relaxed);
  }};
  std::thread second{[&] {
    start.arrive_and_wait();
    accepted.fetch_add(started.source.publish(OperationResult<int>{2}),
                       std::memory_order_relaxed);
  }};
  start.arrive_and_wait();
  first.join();
  second.join();
  started.source.retire();

  const auto result = sync_wait(std::move(started.operation));
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(*result == 1 || *result == 2);
  EXPECT_EQ(accepted.load(std::memory_order_relaxed), 1);
}

TEST(OperationState, AdmissionReleasesAfterObserverAndTransportFinish) {
  auto hooks = std::make_shared<RecordingHooks>();
  auto started = make_operation<int>(hooks);
  ASSERT_TRUE(started.source.publish(OperationResult<int>{47}));

  const auto result = sync_wait(std::move(started.operation));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(hooks->releases.load(std::memory_order_relaxed), 0);

  started.source.retire();
  EXPECT_EQ(hooks->releases.load(std::memory_order_relaxed), 1);
  started.source.retire();
  EXPECT_EQ(hooks->releases.load(std::memory_order_relaxed), 1);
}

TEST(OperationState, DroppingAnOperationDetachesWithoutCancelling) {
  auto hooks = std::make_shared<RecordingHooks>();
  auto started = make_operation<int>(hooks);
  started.operation = {};

  EXPECT_FALSE(started.source.cancel_requested());
  EXPECT_EQ(hooks->wakes.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(hooks->releases.load(std::memory_order_relaxed), 0);
  started.source.retire();
  EXPECT_EQ(hooks->releases.load(std::memory_order_relaxed), 1);
}

TEST(OperationState, TransportPhasesAdvanceBeforeRetirement) {
  auto hooks = std::make_shared<RecordingHooks>();
  auto started = make_operation<int>(hooks);

  started.source.mark_dispatching();
  started.source.mark_active();
  started.source.begin_retirement();
  started.operation = {};
  EXPECT_EQ(hooks->releases.load(std::memory_order_relaxed), 0);

  started.source.retire();
  EXPECT_EQ(hooks->releases.load(std::memory_order_relaxed), 1);
}

TEST(OperationState, SourceMoveAssignmentRetiresItsPreviousTransport) {
  auto first_hooks = std::make_shared<RecordingHooks>();
  auto second_hooks = std::make_shared<RecordingHooks>();
  auto first = make_operation<int>(first_hooks);
  auto second = make_operation<int>(second_hooks);
  first.operation = {};
  second.operation = {};

  first.source = std::move(second.source);
  EXPECT_EQ(first_hooks->releases.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(second_hooks->releases.load(std::memory_order_relaxed), 0);

  first.source.retire();
  EXPECT_EQ(second_hooks->releases.load(std::memory_order_relaxed), 1);
}

struct ThrowingMove final {
  explicit ThrowingMove(std::shared_ptr<bool> fail) : fail{std::move(fail)} {}
  ThrowingMove(const ThrowingMove&) = delete;
  ThrowingMove& operator=(const ThrowingMove&) = delete;
  ThrowingMove(ThrowingMove&& other) : fail{std::move(other.fail)} {
    if (fail && *fail) {
      throw std::runtime_error{"result move"};
    }
  }
  ThrowingMove& operator=(ThrowingMove&&) = delete;

  std::shared_ptr<bool> fail;
};

TEST(OperationState, AThrowingResultMoveStillReleasesAdmission) {
  auto hooks = std::make_shared<RecordingHooks>();
  auto fail = std::make_shared<bool>(false);
  auto started = make_operation<ThrowingMove>(hooks);
  ASSERT_TRUE(
      started.source.publish(OperationResult<ThrowingMove>{ThrowingMove{fail}}));
  started.source.retire();
  *fail = true;

  EXPECT_THROW(static_cast<void>(sync_wait(std::move(started.operation))),
               std::runtime_error);
  EXPECT_EQ(hooks->releases.load(std::memory_order_relaxed), 1);
}

} // namespace
