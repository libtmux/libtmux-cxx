#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "completion_queue.hpp"
#include "move_only_function.hpp"
#include "operation_state.hpp"

namespace {

// Ubuntu's libc++abi frees a std::runtime_error message with free() after
// libc++ allocated it with operator new, which AddressSanitizer reports as a
// mismatch; examples/workspace/CMakeLists.txt describes the same system bug
// where yaml-cpp forces the standard type. Nothing here forces it, and an
// exception that allocates no message keeps the check on.
struct CallerFailure final : std::exception {
  [[nodiscard]] const char* what() const noexcept override { return "caller failure"; }
};

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
using libtmux::detail::OperationCancellation;
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

TEST(CompletionQueue, RunReadyDefersWorkEnqueuedByTheCurrentBatch) {
  CompletionQueue queue;
  const auto first = queue.next_token();
  const auto removed = queue.next_token();
  const auto deferred = queue.next_token();
  const auto mailbox = queue.mailbox();
  std::vector<int> calls;
  ASSERT_TRUE(queue.register_record(first, [&] {
    calls.push_back(1);
    queue.detach(removed);
    EXPECT_TRUE(mailbox.enqueue(deferred));
  }));
  ASSERT_TRUE(queue.register_record(removed, [&] { calls.push_back(2); }));
  ASSERT_TRUE(queue.register_record(deferred, [&] { calls.push_back(3); }));
  ASSERT_TRUE(mailbox.enqueue(first));
  ASSERT_TRUE(mailbox.enqueue(removed));

  EXPECT_EQ(queue.run_ready(), 1U);
  EXPECT_EQ(calls, (std::vector<int>{1}));
  EXPECT_EQ(queue.run_ready(), 1U);
  EXPECT_EQ(calls, (std::vector<int>{1, 3}));
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

TEST(CompletionQueue, PushReadyRegistersAndMakesTheRecordReady) {
  CompletionQueue queue;
  int calls = 0;

  ASSERT_TRUE(queue.push_ready([&] { ++calls; }));

  EXPECT_EQ(queue.run_ready(), 1U);
  EXPECT_EQ(calls, 1);
}

TEST(CompletionQueue, DiscardReadyDoesNotInvokeCallbacks) {
  CompletionQueue queue;
  int calls = 0;
  ASSERT_TRUE(queue.push_ready([&] { ++calls; }));
  ASSERT_TRUE(queue.push_ready([&] { ++calls; }));

  EXPECT_EQ(queue.discard_ready(), 2U);
  EXPECT_EQ(calls, 0);
  EXPECT_EQ(queue.run_ready(), 0U);
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
    throw CallerFailure{};
  }));
  ASSERT_TRUE(queue.register_record(second, [&] { ++calls; }));
  ASSERT_TRUE(queue.mailbox().enqueue(first));
  ASSERT_TRUE(queue.mailbox().enqueue(second));

  EXPECT_THROW(static_cast<void>(queue.run_ready()), CallerFailure);
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

TEST(CompletionQueue, EnqueueAndDetachOverlapLeaveNoReadyRecord) {
  constexpr std::size_t iterations = 512U;
  CompletionQueue queue;
  const auto mailbox = queue.mailbox();
  std::atomic<std::size_t> calls{0U};
  std::vector<CompletionToken> tokens;
  tokens.reserve(iterations);
  for (std::size_t index = 0U; index < iterations; ++index) {
    const auto token = queue.next_token();
    ASSERT_TRUE(queue.register_record(
        token, [&] { calls.fetch_add(1U, std::memory_order_relaxed); }));
    tokens.push_back(token);
  }

  std::barrier phase{3};
  std::thread enqueuer{[&] {
    for (const auto token : tokens) {
      phase.arrive_and_wait();
      static_cast<void>(mailbox.enqueue(token));
      phase.arrive_and_wait();
    }
  }};
  std::thread detacher{[&] {
    for (const auto token : tokens) {
      phase.arrive_and_wait();
      mailbox.detach(token);
      phase.arrive_and_wait();
    }
  }};

  for (std::size_t index = 0U; index < iterations; ++index) {
    phase.arrive_and_wait();
    phase.arrive_and_wait();
    EXPECT_EQ(queue.run_ready(), 0U);
  }
  enqueuer.join();
  detacher.join();
  EXPECT_EQ(calls.load(std::memory_order_relaxed), 0U);
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

class QueueReenteringHooks final : public OperationHooks {
public:
  explicit QueueReenteringHooks(CompletionQueue& queue) noexcept : queue_{queue} {}

  void wake_reactor() noexcept override {}

  void release_admission() noexcept override {
    static_cast<void>(queue_.next_token());
    ++releases;
  }

  int releases{0};

private:
  CompletionQueue& queue_;
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

TEST(OperationCallback, PublicationAndRegistrationOverlapDeliverOnce) {
  constexpr std::size_t iterations = 256U;
  CompletionQueue queue;
  std::atomic<std::size_t> generation{0U};
  std::atomic<std::size_t> subscribed{0U};
  std::atomic<std::size_t> published_generation{0U};
  libtmux::detail::StartedOperation<int>* current = nullptr;
  std::optional<Subscription<int>> subscription;
  bool published = false;
  int observed = -1;
  const auto wait_for = [](std::atomic<std::size_t>& counter, std::size_t expected) {
    auto observed = counter.load(std::memory_order_acquire);
    while (observed < expected) {
      counter.wait(observed, std::memory_order_acquire);
      observed = counter.load(std::memory_order_acquire);
    }
  };

  std::thread subscriber{[&] {
    for (std::size_t index = 0U; index < iterations; ++index) {
      wait_for(generation, index + 1U);
      subscription.emplace(
          std::move(current->operation)
              .subscribe(queue, [&, expected = static_cast<int>(index)](
                                    OperationResult<int> result) {
                EXPECT_TRUE(result.has_value());
                if (result) {
                  EXPECT_EQ(*result, expected);
                  observed = *result;
                }
              }));
      subscribed.store(index + 1U, std::memory_order_release);
      subscribed.notify_one();
    }
  }};
  std::thread publisher{[&] {
    for (std::size_t index = 0U; index < iterations; ++index) {
      wait_for(generation, index + 1U);
      published =
          current->source.publish(OperationResult<int>{static_cast<int>(index)});
      published_generation.store(index + 1U, std::memory_order_release);
      published_generation.notify_one();
    }
  }};

  for (std::size_t index = 0U; index < iterations; ++index) {
    auto hooks = std::make_shared<RecordingHooks>();
    auto started = make_operation<int>(hooks);
    current = &started;
    published = false;
    observed = -1;
    generation.store(index + 1U, std::memory_order_release);
    generation.notify_all();
    wait_for(subscribed, index + 1U);
    wait_for(published_generation, index + 1U);

    EXPECT_TRUE(published);
    EXPECT_EQ(queue.run_ready(), 1U);
    EXPECT_EQ(observed, static_cast<int>(index));
    started.source.retire();
    EXPECT_EQ(hooks->releases.load(std::memory_order_relaxed), 1);
    subscription.reset();
  }
  subscriber.join();
  publisher.join();
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
        throw CallerFailure{};
      });
  ASSERT_TRUE(started.source.publish(OperationResult<int>{73}));
  started.source.retire();

  EXPECT_THROW(static_cast<void>(queue.run_ready()), CallerFailure);
  EXPECT_EQ(queue.run_ready(), 0U);
  EXPECT_FALSE(subscription.observing());
  EXPECT_EQ(hooks->releases.load(std::memory_order_relaxed), 1);
}

TEST(OperationCallback, ThrowingObserverReleasesOwnershipBeforeUserCode) {
  CompletionQueue queue;
  auto first_hooks = std::make_shared<RecordingHooks>();
  auto second_hooks = std::make_shared<RecordingHooks>();
  std::weak_ptr<RecordingHooks> first_watched = first_hooks;
  auto first = make_operation<int>(first_hooks);
  auto second = make_operation<int>(second_hooks);
  int later_calls = 0;
  auto first_subscription =
      std::move(first.operation).subscribe(queue, [&](OperationResult<int>) {
        EXPECT_TRUE(first_watched.expired());
        throw CallerFailure{};
      });
  auto second_subscription =
      std::move(second.operation).subscribe(queue, [&](OperationResult<int>) {
        ++later_calls;
      });
  ASSERT_TRUE(first.source.publish(OperationResult<int>{79}));
  ASSERT_TRUE(second.source.publish(OperationResult<int>{83}));
  first.source.retire();
  second.source.retire();
  first.source = {};
  second.source = {};
  first_hooks.reset();

  EXPECT_THROW(static_cast<void>(queue.run_ready()), CallerFailure);
  EXPECT_EQ(queue.run_ready(), 1U);
  EXPECT_EQ(later_calls, 1);
  EXPECT_FALSE(first_subscription.observing());
  EXPECT_FALSE(second_subscription.observing());
  EXPECT_EQ(second_hooks->releases.load(std::memory_order_relaxed), 1);
}

TEST(OperationCallback, DiscardReadyReleasesObserverWithoutCallingIt) {
  CompletionQueue queue;
  auto hooks = std::make_shared<QueueReenteringHooks>(queue);
  auto started = make_operation<int>(hooks);
  int calls = 0;
  auto subscription =
      std::move(started.operation).subscribe(queue, [&](OperationResult<int>) {
        ++calls;
      });
  ASSERT_TRUE(started.source.publish(OperationResult<int>{87}));
  started.source.retire();

  EXPECT_EQ(queue.discard_ready(), 1U);
  EXPECT_EQ(calls, 0);
  EXPECT_FALSE(subscription.observing());
  EXPECT_EQ(hooks->releases, 1);
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

TEST(OperationState, DefaultOperationHasAnInertCancellationHandle) {
  const Operation<int> operation;

  EXPECT_FALSE(operation.cancellation().request_cancel());
}

TEST(OperationState, MovedFromOperationHasAnInertCancellationHandle) {
  auto hooks = std::make_shared<RecordingHooks>();
  auto started = make_operation<int>(hooks);
  const Operation<int> owner{std::move(started.operation)};

  EXPECT_FALSE(started.operation.cancellation().request_cancel());
}

TEST(OperationState, CancellationIsOnlyARequestUntilTheSourcePublishes) {
  auto hooks = std::make_shared<RecordingHooks>();
  auto started = make_operation<int>(hooks);

  EXPECT_TRUE(started.operation.request_cancel());
  EXPECT_FALSE(started.operation.request_cancel());
  EXPECT_EQ(hooks->wakes.load(std::memory_order_relaxed), 1);
  EXPECT_FALSE(started.source.outcome_published());

  ASSERT_TRUE(started.source.publish(
      unexpected(libtmux::CommandFailure{.kind = FailureKind::timeout,
                                         .delivery = DeliveryStatus::not_started,
                                         .diagnostic = {}})));
  started.source.retire();
  EXPECT_EQ(hooks->releases.load(std::memory_order_relaxed), 0);
  const auto result = sync_wait(std::move(started.operation));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().kind, FailureKind::timeout);
  EXPECT_EQ(hooks->releases.load(std::memory_order_relaxed), 1);
}

TEST(OperationState, CancellationHandleExpiresWithoutATrueOwner) {
  OperationCancellation<std::shared_ptr<int>> cancellation;
  std::weak_ptr<int> watched;
  {
    auto hooks = std::make_shared<RecordingHooks>();
    auto payload = std::make_shared<int>(89);
    watched = payload;
    auto started = make_operation<std::shared_ptr<int>>(hooks);
    cancellation = started.operation.cancellation();
    ASSERT_TRUE(started.source.publish(
        OperationResult<std::shared_ptr<int>>{std::move(payload)}));
    started.operation = {};
    started.source = {};
  }

  EXPECT_TRUE(watched.expired()) << "the cancellation relay retained operation state";
  EXPECT_FALSE(cancellation.request_cancel());
}

TEST(OperationState, AReplyCanBeatARequestedCancellation) {
  auto hooks = std::make_shared<RecordingHooks>();
  auto started = make_operation<int>(hooks);
  ASSERT_TRUE(started.operation.request_cancel());

  EXPECT_TRUE(started.source.publish(OperationResult<int>{43}));
  EXPECT_FALSE(started.source.publish(
      unexpected(libtmux::CommandFailure{.kind = FailureKind::timeout,
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
  std::promise<OperationResult<int>> answer;
  auto waited = answer.get_future();
  std::thread waiter{[operation = std::move(started.operation), &answer]() mutable {
    answer.set_value(sync_wait(std::move(operation)));
  }};

  while (!started.source.blocking_observer_waiting()) {
    std::this_thread::yield();
  }
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

TEST(OperationState, RetirementWakesABlockedObserver) {
  auto hooks = std::make_shared<RecordingHooks>();
  auto started = make_operation<int>(hooks);
  std::promise<OperationResult<int>> answer;
  auto waited = answer.get_future();
  std::thread waiter{[operation = std::move(started.operation), &answer]() mutable {
    answer.set_value(sync_wait(std::move(operation)));
  }};

  while (!started.source.blocking_observer_waiting()) {
    std::this_thread::yield();
  }
  started.source.retire();
  const auto result = waited.get();
  waiter.join();

  EXPECT_TRUE(started.source.outcome_published());
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
      throw CallerFailure{};
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
      throw CallerFailure{};
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
               CallerFailure);
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
