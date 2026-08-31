#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "completion_queue.hpp"
#include "operation_state.hpp"

namespace {

thread_local std::size_t allocations_before_failure =
    std::numeric_limits<std::size_t>::max();
thread_local bool block_next_allocation = false;
std::atomic_bool allocation_blocked{false};
std::atomic_bool release_allocation{false};

[[nodiscard]] void* allocate(std::size_t size) {
  if (std::exchange(block_next_allocation, false)) {
    allocation_blocked.store(true, std::memory_order_release);
    allocation_blocked.notify_one();
    release_allocation.wait(false, std::memory_order_acquire);
  }
  if (allocations_before_failure == 0U) {
    throw std::bad_alloc{};
  }
  if (allocations_before_failure != std::numeric_limits<std::size_t>::max()) {
    --allocations_before_failure;
  }
  if (void* allocation = std::malloc(size == 0U ? 1U : size)) {
    return allocation;
  }
  throw std::bad_alloc{};
}

class AllocationFailure final {
public:
  explicit AllocationFailure(std::size_t successful_allocations) noexcept
      : previous_{std::exchange(allocations_before_failure, successful_allocations)} {}

  ~AllocationFailure() { allocations_before_failure = previous_; }

  AllocationFailure(const AllocationFailure&) = delete;
  AllocationFailure& operator=(const AllocationFailure&) = delete;

private:
  std::size_t previous_;
};

void block_this_threads_next_allocation() noexcept {
  allocation_blocked.store(false, std::memory_order_relaxed);
  release_allocation.store(false, std::memory_order_relaxed);
  block_next_allocation = true;
}

void wait_until_allocation_blocks() noexcept {
  while (!allocation_blocked.load(std::memory_order_acquire)) {
    allocation_blocked.wait(false, std::memory_order_acquire);
  }
}

void release_blocked_allocation() noexcept {
  release_allocation.store(true, std::memory_order_release);
  release_allocation.notify_one();
}

struct ReenterOnDestroy final {
  ReenterOnDestroy(libtmux::detail::CompletionQueue* queue, bool* destroyed) noexcept
      : queue{queue}, destroyed{destroyed} {}

  ~ReenterOnDestroy() {
    static_cast<void>(queue->next_token());
    *destroyed = true;
  }

  libtmux::detail::CompletionQueue* queue;
  bool* destroyed;
};

[[nodiscard]] int enqueue_without_allocating() {
  libtmux::detail::CompletionQueue queue;
  std::vector<libtmux::detail::CompletionToken> tokens;
  tokens.reserve(4096U);
  for (std::size_t index = 0U; index < tokens.capacity(); ++index) {
    const auto token = queue.next_token();
    if (!queue.register_record(token, [] {})) {
      return 2;
    }
    tokens.push_back(token);
  }

  {
    AllocationFailure fail_next{0U};
    for (const auto token : tokens) {
      if (!queue.mailbox().enqueue(token)) {
        return 3;
      }
    }
  }

  for (const auto token : tokens) {
    queue.detach(token);
  }
  return 0;
}

[[nodiscard]] int insertion_failure_destroys_after_unlock() {
  libtmux::detail::CompletionQueue queue;
  const auto token = queue.next_token();
  bool destroyed = false;
  libtmux::detail::MoveOnlyFunction<void()> callback{
      [capture = std::make_unique<ReenterOnDestroy>(&queue, &destroyed)] {}};
  if (destroyed) {
    return 5;
  }

  bool failed = false;
  try {
    AllocationFailure fail_second{1U};
    static_cast<void>(queue.register_record(token, std::move(callback)));
  } catch (const std::bad_alloc&) {
    failed = true;
  }
  return failed && destroyed ? 0 : 4;
}

class RecordingHooks final : public libtmux::detail::OperationHooks {
public:
  void wake_reactor() noexcept override {}
  void release_admission() noexcept override { ++releases; }

  std::atomic<int> releases{0};
};

[[nodiscard]] int publication_during_registration_rechecks() {
  using libtmux::detail::OperationCallback;
  using libtmux::detail::OperationResult;
  using libtmux::detail::Subscription;

  libtmux::detail::CompletionQueue queue;
  auto hooks = std::make_shared<RecordingHooks>();
  auto started = libtmux::detail::make_operation<int>(hooks);
  std::optional<Subscription<int>> subscription;
  std::atomic<int> observed{-1};
  std::thread subscriber{[operation = std::move(started.operation), &queue,
                          &subscription, &observed]() mutable {
    OperationCallback<int> callback{[&observed](OperationResult<int> result) {
      if (result) {
        observed.store(*result, std::memory_order_relaxed);
      }
    }};
    block_this_threads_next_allocation();
    subscription.emplace(std::move(operation).subscribe(queue, std::move(callback)));
  }};

  wait_until_allocation_blocks();
  const bool published = started.source.publish(OperationResult<int>{61});
  release_blocked_allocation();
  subscriber.join();

  if (!published || queue.run_ready() != 1U ||
      observed.load(std::memory_order_relaxed) != 61) {
    return 6;
  }
  started.source.retire();
  subscription.reset();
  return hooks->releases.load(std::memory_order_relaxed) == 1 ? 0 : 7;
}

[[nodiscard]] int atomic_ready_insertion_dispatches() {
  libtmux::detail::CompletionQueue queue;
  std::atomic_bool callback_ran{false};
  std::atomic_bool inserted{false};
  std::atomic_bool dispatched{false};
  libtmux::detail::MoveOnlyFunction<void()> callback{
      [&] { callback_ran.store(true, std::memory_order_release); }};

  std::thread producer{[&] {
    block_this_threads_next_allocation();
    inserted.store(queue.push_ready(std::move(callback)), std::memory_order_release);
  }};
  wait_until_allocation_blocks();
  std::thread consumer{
      [&] { dispatched.store(queue.run_one(), std::memory_order_release); }};
  release_blocked_allocation();
  producer.join();
  consumer.join();

  return inserted.load(std::memory_order_acquire) &&
                 dispatched.load(std::memory_order_acquire) &&
                 callback_ran.load(std::memory_order_acquire)
             ? 0
             : 8;
}

} // namespace

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete[](void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { std::free(allocation); }
void operator delete[](void* allocation, std::size_t) noexcept {
  std::free(allocation);
}

int main(int argc, char** argv) {
  if (argc != 2) {
    return 1;
  }
  const std::string_view mode{argv[1]};
  if (mode == "enqueue") {
    return enqueue_without_allocating();
  }
  if (mode == "insertion") {
    return insertion_failure_destroys_after_unlock();
  }
  if (mode == "registration") {
    return publication_during_registration_rechecks();
  }
  if (mode == "ready") {
    return atomic_ready_insertion_dispatches();
  }
  return 1;
}
