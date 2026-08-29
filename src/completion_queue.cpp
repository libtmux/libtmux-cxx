#include "completion_queue.hpp"

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

LIBTMUX_NAMESPACE_BEGIN
namespace detail {

class CompletionQueueCore final {
public:
  struct Record final {
    explicit Record(MoveOnlyFunction<void()> callback) noexcept
        : callback{std::move(callback)} {}

    MoveOnlyFunction<void()> callback;
    bool enqueued{false};
    std::optional<std::uint64_t> previous_ready;
    std::optional<std::uint64_t> next_ready;
  };

  using Records = std::unordered_map<std::uint64_t, std::unique_ptr<Record>>;

  void link_ready(std::uint64_t token, Record& record) noexcept {
    assert(!record.enqueued);
    record.enqueued = true;
    record.previous_ready = ready_tail;
    if (ready_tail) {
      const auto previous = records.find(*ready_tail);
      assert(previous != records.end());
      previous->second->next_ready = token;
    } else {
      ready_head = token;
    }
    ready_tail = token;
    ++ready_count;
  }

  void unlink_ready(std::uint64_t token, Record& record) noexcept {
    assert(record.enqueued);
    if (record.previous_ready) {
      const auto previous = records.find(*record.previous_ready);
      assert(previous != records.end());
      previous->second->next_ready = record.next_ready;
    } else {
      assert(ready_head == token);
      ready_head = record.next_ready;
    }
    if (record.next_ready) {
      const auto next = records.find(*record.next_ready);
      assert(next != records.end());
      next->second->previous_ready = record.previous_ready;
    } else {
      assert(ready_tail == token);
      ready_tail = record.previous_ready;
    }
    record.enqueued = false;
    record.previous_ready.reset();
    record.next_ready.reset();
    assert(ready_count != 0U);
    --ready_count;
  }

  [[nodiscard]] Records::node_type claim_ready() noexcept {
    if (!ready_head) {
      return {};
    }
    const auto record = records.find(*ready_head);
    assert(record != records.end());
    const auto token = record->first;
    unlink_ready(token, *record->second);
    return records.extract(record);
  }

  std::mutex mutex;
  std::condition_variable ready_changed;
  Records records;
  std::optional<std::uint64_t> ready_head;
  std::optional<std::uint64_t> ready_tail;
  std::size_t ready_count{0U};
  std::uint64_t next_token{1U};
  bool closed{false};
  std::atomic_bool dispatching{false};
};

class DispatchClaim final {
public:
  explicit DispatchClaim(std::atomic_bool& dispatching) noexcept
      : dispatching_{dispatching} {
    bool available = false;
    owns_ = dispatching_.compare_exchange_strong(
        available, true, std::memory_order_acquire, std::memory_order_relaxed);
  }

  ~DispatchClaim() {
    if (owns_) {
      dispatching_.store(false, std::memory_order_release);
    }
  }

  DispatchClaim(const DispatchClaim&) = delete;
  DispatchClaim& operator=(const DispatchClaim&) = delete;

  [[nodiscard]] bool owns() const noexcept { return owns_; }

private:
  std::atomic_bool& dispatching_;
  bool owns_{false};
};

WeakCompletionMailbox::WeakCompletionMailbox(
    std::weak_ptr<CompletionQueueCore> core) noexcept
    : core_{std::move(core)} {}

bool WeakCompletionMailbox::enqueue(CompletionToken token) const noexcept {
  const auto core = core_.lock();
  if (!core) {
    return false;
  }

  {
    std::unique_lock lock{core->mutex};
    const auto record = core->records.find(token.value);
    if (core->closed || record == core->records.end() || record->second->enqueued) {
      return false;
    }
    core->link_ready(token.value, *record->second);
  }
  core->ready_changed.notify_one();
  return true;
}

void WeakCompletionMailbox::detach(CompletionToken token) const noexcept {
  const auto core = core_.lock();
  if (!core) {
    return;
  }

  CompletionQueueCore::Records::node_type detached;
  {
    std::unique_lock lock{core->mutex};
    const auto record = core->records.find(token.value);
    if (record == core->records.end()) {
      return;
    }
    if (record->second->enqueued) {
      core->unlink_ready(token.value, *record->second);
    }
    detached = core->records.extract(record);
  }
}

CompletionQueue::CompletionQueue() : core_{std::make_shared<CompletionQueueCore>()} {}

CompletionQueue::~CompletionQueue() { close(); }

CompletionToken CompletionQueue::next_token() noexcept {
  const auto core = core_;
  std::unique_lock lock{core->mutex};
  return CompletionToken{core->next_token++};
}

WeakCompletionMailbox CompletionQueue::mailbox() const noexcept {
  return WeakCompletionMailbox{core_};
}

bool CompletionQueue::register_record(CompletionToken token,
                                      MoveOnlyFunction<void()> callback) {
  const auto core = core_;
  auto record = std::make_unique<CompletionQueueCore::Record>(std::move(callback));
  {
    std::unique_lock lock{core->mutex};
    if (core->closed) {
      return false;
    }
    const auto [position, inserted] = core->records.try_emplace(token.value);
    if (!inserted) {
      return false;
    }
    position->second = std::move(record);
  }
  return true;
}

bool CompletionQueue::run_one() {
  const auto core = core_;
  DispatchClaim dispatch{core->dispatching};
  if (!dispatch.owns()) {
    return false;
  }

  CompletionQueueCore::Records::node_type claimed;
  {
    std::unique_lock lock{core->mutex};
    while (claimed.empty()) {
      core->ready_changed.wait(lock,
                               [&] { return core->closed || core->ready_count != 0U; });
      if (core->ready_count != 0U) {
        claimed = core->claim_ready();
      }
      if (core->closed) {
        break;
      }
    }
  }
  if (claimed.empty()) {
    return false;
  }

  claimed.mapped()->callback();
  return true;
}

std::size_t CompletionQueue::run_ready() {
  const auto core = core_;
  DispatchClaim dispatch{core->dispatching};
  if (!dispatch.owns()) {
    return 0U;
  }

  std::size_t ready_count = 0U;
  {
    std::unique_lock lock{core->mutex};
    ready_count = core->ready_count;
  }

  std::size_t dispatched = 0U;
  for (std::size_t index = 0U; index < ready_count; ++index) {
    CompletionQueueCore::Records::node_type claimed;
    {
      std::unique_lock lock{core->mutex};
      if (core->ready_count == 0U) {
        break;
      }
      claimed = core->claim_ready();
    }
    claimed.mapped()->callback();
    ++dispatched;
  }
  return dispatched;
}

void CompletionQueue::detach(CompletionToken token) { mailbox().detach(token); }

void CompletionQueue::close() {
  const auto core = core_;
  CompletionQueueCore::Records records;
  {
    std::unique_lock lock{core->mutex};
    if (core->closed) {
      return;
    }
    core->closed = true;
    core->ready_head.reset();
    core->ready_tail.reset();
    core->ready_count = 0U;
    records.swap(core->records);
  }
  core->ready_changed.notify_all();
}

} // namespace detail
LIBTMUX_NAMESPACE_END
