#include <cstddef>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

#include "completion_queue.hpp"

namespace {

thread_local std::size_t allocations_before_failure =
    std::numeric_limits<std::size_t>::max();

[[nodiscard]] void* allocate(std::size_t size) {
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

struct ReenterOnDestroy final {
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
      [capture = std::make_unique<ReenterOnDestroy>(ReenterOnDestroy{
           .queue = &queue,
           .destroyed = &destroyed,
       })] {}};

  bool failed = false;
  try {
    AllocationFailure fail_second{1U};
    static_cast<void>(queue.register_record(token, std::move(callback)));
  } catch (const std::bad_alloc&) {
    failed = true;
  }
  return failed && destroyed ? 0 : 4;
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
  return 1;
}
