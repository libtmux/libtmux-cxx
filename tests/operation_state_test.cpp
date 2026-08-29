#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <functional>
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
using libtmux::detail::Subscription;
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

TEST(CompletionQueue, DetachingReadyRecordsDoesNotObstructLaterDispatch) {
  CompletionQueue queue;
  for (std::size_t index = 0U; index < 4096U; ++index) {
    const auto token = queue.next_token();
    ASSERT_TRUE(queue.register_record(token, [] {}));
    ASSERT_TRUE(queue.mailbox().enqueue(token));
    queue.detach(token);
  }

  int calls = 0;
  const auto token = queue.next_token();
  ASSERT_TRUE(queue.register_record(token, [&] { ++calls; }));
  ASSERT_TRUE(queue.mailbox().enqueue(token));
  EXPECT_TRUE(queue.run_one());
  EXPECT_EQ(calls, 1);
}

TEST(CompletionQueue, CallbackReentryDoesNotOverlapDispatch) {
  CompletionQueue queue;
  const auto first = queue.next_token();
  const auto second = queue.next_token();
  std::size_t nested = 1U;
  ASSERT_TRUE(queue.register_record(first, [&] { nested = queue.run_ready(); }));
  ASSERT_TRUE(queue.register_record(second, [] {}));
  ASSERT_TRUE(queue.mailbox().enqueue(first));
  ASSERT_TRUE(queue.mailbox().enqueue(second));

  EXPECT_TRUE(queue.run_one());
  EXPECT_EQ(nested, 0U);
  EXPECT_EQ(queue.run_ready(), 1U);
}

struct ReenterQueueOnDestroy final {
  ~ReenterQueueOnDestroy() { *nested = queue->run_ready(); }

  CompletionQueue* queue;
  std::size_t* nested;
};

TEST(CompletionQueue, CaptureDestructionDoesNotOverlapDispatch) {
  CompletionQueue queue;
  const auto first = queue.next_token();
  const auto second = queue.next_token();
  std::size_t nested = 1U;
  auto capture = std::make_shared<ReenterQueueOnDestroy>(
      ReenterQueueOnDestroy{.queue = &queue, .nested = &nested});
  ASSERT_TRUE(queue.register_record(first, [capture] {}));
  capture.reset();
  ASSERT_TRUE(queue.register_record(second, [] {}));
  ASSERT_TRUE(queue.mailbox().enqueue(first));
  ASSERT_TRUE(queue.mailbox().enqueue(second));

  EXPECT_TRUE(queue.run_one());
  EXPECT_EQ(nested, 0U);
  EXPECT_EQ(queue.run_ready(), 1U);
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

TEST(OperationCallback, PublicationBeforeSubscribeSurvivesTheRecheck) {
  CompletionQueue queue;
  auto hooks = std::make_shared<RecordingHooks>();
  auto started = make_operation<int>(hooks);
  ASSERT_TRUE(started.source.publish(OperationResult<int>{53}));

  std::optional<int> observed;
  [[maybe_unused]] auto subscription =
      std::move(started.operation).subscribe(queue, [&](OperationResult<int> result) {
        ASSERT_TRUE(result.has_value());
        observed = *result;
      });

  EXPECT_FALSE(observed.has_value());
  EXPECT_EQ(queue.run_ready(), 1U);
  EXPECT_EQ(observed, 53);
  started.source.retire();
  EXPECT_EQ(hooks->releases.load(std::memory_order_relaxed), 1);
}

TEST(OperationCallback, MovesOnePayloadIntoOneCallback) {
  CompletionQueue queue;
  auto hooks = std::make_shared<RecordingHooks>();
  auto started = make_operation<std::unique_ptr<int>>(hooks);
  std::unique_ptr<int> observed;
  int calls = 0;
  [[maybe_unused]] auto subscription =
      std::move(started.operation)
          .subscribe(queue, [owned = std::make_unique<int>(1), &observed,
                             &calls](OperationResult<std::unique_ptr<int>> result) {
            EXPECT_EQ(*owned, 1);
            observed = std::move(*result);
            ++calls;
          });

  ASSERT_TRUE(started.source.publish(
      OperationResult<std::unique_ptr<int>>{std::make_unique<int>(59)}));
  EXPECT_EQ(calls, 0);
  EXPECT_EQ(queue.run_ready(), 1U);
  ASSERT_NE(observed, nullptr);
  EXPECT_EQ(*observed, 59);
  EXPECT_EQ(calls, 1);
  started.source.retire();
}

TEST(OperationCallback, DetachDoesNotRequestCancellation) {
  CompletionQueue queue;
  auto hooks = std::make_shared<RecordingHooks>();
  auto started = make_operation<int>(hooks);
  int calls = 0;
  [[maybe_unused]] auto subscription =
      std::move(started.operation).subscribe(queue, [&](OperationResult<int>) {
        ++calls;
      });

  subscription.detach();
  EXPECT_FALSE(started.source.cancel_requested());
  ASSERT_TRUE(started.source.publish(OperationResult<int>{61}));
  EXPECT_EQ(queue.run_ready(), 0U);
  EXPECT_EQ(calls, 0);
  started.source.retire();
  EXPECT_EQ(hooks->releases.load(std::memory_order_relaxed), 1);
}

TEST(OperationCallback, MoveAssignmentDetachesThePreviousObserver) {
  CompletionQueue queue;
  auto first_hooks = std::make_shared<RecordingHooks>();
  auto second_hooks = std::make_shared<RecordingHooks>();
  auto first = make_operation<int>(first_hooks);
  auto second = make_operation<int>(second_hooks);
  int calls = 0;
  auto subscription =
      std::move(first.operation).subscribe(queue, [&](OperationResult<int>) {
        ++calls;
      });
  auto replacement =
      std::move(second.operation).subscribe(queue, [&](OperationResult<int>) {
        ++calls;
      });

  subscription = std::move(replacement);
  EXPECT_FALSE(replacement.observing());
  ASSERT_TRUE(first.source.publish(OperationResult<int>{63}));
  EXPECT_EQ(queue.run_ready(), 0U);
  first.source.retire();
  EXPECT_EQ(first_hooks->releases.load(std::memory_order_relaxed), 1);

  ASSERT_TRUE(second.source.publish(OperationResult<int>{65}));
  EXPECT_EQ(queue.run_ready(), 1U);
  EXPECT_EQ(calls, 1);
  second.source.retire();
  EXPECT_EQ(second_hooks->releases.load(std::memory_order_relaxed), 1);
}

TEST(OperationCallback, QueueCloseDoesNotPublishCancellation) {
  CompletionQueue queue;
  auto hooks = std::make_shared<RecordingHooks>();
  auto started = make_operation<int>(hooks);
  int calls = 0;
  auto subscription =
      std::move(started.operation).subscribe(queue, [&](OperationResult<int>) {
        ++calls;
      });

  queue.close();
  EXPECT_FALSE(subscription.observing());
  EXPECT_FALSE(started.source.cancel_requested());
  EXPECT_FALSE(started.source.outcome_published());
  ASSERT_TRUE(started.source.publish(OperationResult<int>{67}));
  EXPECT_EQ(calls, 0);
  started.source.retire();
  EXPECT_EQ(hooks->releases.load(std::memory_order_relaxed), 1);
}

TEST(OperationCallback, CancellationRequestCanLoseToAReply) {
  CompletionQueue queue;
  auto hooks = std::make_shared<RecordingHooks>();
  auto started = make_operation<int>(hooks);
  std::optional<OperationResult<int>> observed;
  auto subscription =
      std::move(started.operation).subscribe(queue, [&](OperationResult<int> result) {
        observed.emplace(std::move(result));
      });

  ASSERT_TRUE(subscription.request_cancel());
  EXPECT_FALSE(started.source.outcome_published());
  ASSERT_TRUE(started.source.publish(OperationResult<int>{71}));
  ASSERT_EQ(queue.run_ready(), 1U);
  ASSERT_TRUE(observed.has_value());
  ASSERT_TRUE(observed->has_value());
  EXPECT_EQ(**observed, 71);
  started.source.retire();
}

TEST(OperationCallback, CallbackExceptionsKeepDeliveryTerminal) {
  CompletionQueue queue;
  auto hooks = std::make_shared<RecordingHooks>();
  auto started = make_operation<int>(hooks);
  [[maybe_unused]] auto subscription =
      std::move(started.operation).subscribe(queue, [](OperationResult<int>) {
        throw std::runtime_error{"caller failure"};
      });
  ASSERT_TRUE(started.source.publish(OperationResult<int>{73}));
  started.source.retire();

  EXPECT_THROW(static_cast<void>(queue.run_ready()), std::runtime_error);
  EXPECT_EQ(queue.run_ready(), 0U);
  EXPECT_FALSE(subscription.observing());
  EXPECT_EQ(hooks->releases.load(std::memory_order_relaxed), 1);
}

TEST(OperationCallback, SourceDestructionPublishesTransportFailure) {
  CompletionQueue queue;
  auto hooks = std::make_shared<RecordingHooks>();
  auto started = make_operation<int>(hooks);
  std::optional<OperationResult<int>> observed;
  auto subscription =
      std::move(started.operation).subscribe(queue, [&](OperationResult<int> result) {
        observed.emplace(std::move(result));
      });

  { auto source = std::move(started.source); }

  EXPECT_EQ(queue.run_ready(), 1U);
  ASSERT_TRUE(observed.has_value());
  ASSERT_FALSE(observed->has_value());
  EXPECT_EQ(observed->error().kind, FailureKind::pipe);
  EXPECT_EQ(observed->error().delivery, DeliveryStatus::not_started);
  EXPECT_EQ(observed->error().diagnostic,
            "operation source retired without an outcome");
  EXPECT_FALSE(subscription.observing());
  EXPECT_EQ(hooks->releases.load(std::memory_order_relaxed), 1);
}

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

TEST(OperationState, RetirementPublishesFailureToBlockingObserver) {
  auto hooks = std::make_shared<RecordingHooks>();
  auto started = make_operation<int>(hooks);

  started.source.retire();
  ASSERT_TRUE(started.source.outcome_published());
  const auto result = sync_wait(std::move(started.operation));

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().kind, FailureKind::pipe);
  EXPECT_EQ(result.error().delivery, DeliveryStatus::not_started);
  EXPECT_EQ(result.error().diagnostic, "operation source retired without an outcome");
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
  CompletionQueue queue;
  std::optional<OperationResult<int>> observed;
  auto subscription =
      std::move(first.operation).subscribe(queue, [&](OperationResult<int> result) {
        observed.emplace(std::move(result));
      });
  second.operation = {};

  first.source = std::move(second.source);
  EXPECT_EQ(queue.run_ready(), 1U);
  ASSERT_TRUE(observed.has_value());
  ASSERT_FALSE(observed->has_value());
  EXPECT_EQ(observed->error().kind, FailureKind::pipe);
  EXPECT_FALSE(subscription.observing());
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

struct CountedThrowingMove final {
  CountedThrowingMove(std::shared_ptr<int> moves, std::shared_ptr<int> throw_on)
      : moves{std::move(moves)}, throw_on{std::move(throw_on)} {}
  CountedThrowingMove(const CountedThrowingMove&) = delete;
  CountedThrowingMove& operator=(const CountedThrowingMove&) = delete;
  CountedThrowingMove(CountedThrowingMove&& other)
      : moves{other.moves}, throw_on{other.throw_on} {
    const int move = ++*moves;
    if (move == *throw_on) {
      throw std::runtime_error{"result move"};
    }
  }
  CountedThrowingMove& operator=(CountedThrowingMove&&) = delete;

  std::shared_ptr<int> moves;
  std::shared_ptr<int> throw_on;
};

struct ReenteringMove final {
  explicit ReenteringMove(std::shared_ptr<std::function<void()>> on_move)
      : on_move{std::move(on_move)} {}
  ReenteringMove(const ReenteringMove&) = delete;
  ReenteringMove& operator=(const ReenteringMove&) = delete;
  ReenteringMove(ReenteringMove&& other) : on_move{other.on_move} {
    if (*on_move) {
      (*on_move)();
    }
  }
  ReenteringMove& operator=(ReenteringMove&&) = delete;

  std::shared_ptr<std::function<void()>> on_move;
};

TEST(OperationState, PublicationDoesNotMoveAWholeExpected) {
  auto hooks = std::make_shared<RecordingHooks>();
  auto moves = std::make_shared<int>(0);
  auto throw_on = std::make_shared<int>(0);
  auto started = make_operation<CountedThrowingMove>(hooks);
  OperationResult<CountedThrowingMove> pending{CountedThrowingMove{moves, throw_on}};
  *throw_on = *moves + 2;

  bool published = false;
  EXPECT_NO_THROW(published = started.source.publish(std::move(pending)));
  ASSERT_TRUE(published);
  *throw_on = 0;
  started.source.retire();

  const auto result = sync_wait(std::move(started.operation));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(hooks->releases.load(std::memory_order_relaxed), 1);
}

TEST(OperationState, PublicationMovesUserValuesWithoutTheStateLock) {
  auto hooks = std::make_shared<RecordingHooks>();
  auto on_move = std::make_shared<std::function<void()>>();
  auto started = make_operation<ReenteringMove>(hooks);
  OperationResult<ReenteringMove> pending{ReenteringMove{on_move}};
  *on_move = [&] { static_cast<void>(started.source.cancel_requested()); };

  ASSERT_TRUE(started.source.publish(std::move(pending)));
  started.source.retire();
  const auto result = sync_wait(std::move(started.operation));

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(hooks->releases.load(std::memory_order_relaxed), 1);
}

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

TEST(OperationCallback, DoesNotMoveTheWholeResultAtTheFinalCallBoundary) {
  CompletionQueue queue;
  auto hooks = std::make_shared<RecordingHooks>();
  auto moves = std::make_shared<int>(0);
  auto throw_on = std::make_shared<int>(0);
  auto started = make_operation<CountedThrowingMove>(hooks);
  int calls = 0;
  auto subscription =
      std::move(started.operation)
          .subscribe(queue, [&](OperationResult<CountedThrowingMove>) { ++calls; });
  ASSERT_TRUE(started.source.publish(
      OperationResult<CountedThrowingMove>{CountedThrowingMove{moves, throw_on}}));
  started.source.retire();
  *throw_on = *moves + 4;

  EXPECT_NO_THROW(static_cast<void>(queue.run_ready()));
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(queue.run_ready(), 0U);
  EXPECT_FALSE(subscription.observing());
  EXPECT_EQ(hooks->releases.load(std::memory_order_relaxed), 1);
}

} // namespace
