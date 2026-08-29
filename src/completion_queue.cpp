#include "completion_queue.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <utility>

LIBTMUX_NAMESPACE_BEGIN
namespace detail {

class CompletionQueueCore final {
public:
  struct Record final {
    MoveOnlyFunction<void()> callback;
    bool enqueued{false};
  };

  std::mutex mutex;
  std::condition_variable ready_changed;
  std::unordered_map<std::uint64_t, Record> records;
  std::deque<std::uint64_t> ready;
  std::uint64_t next_token{1U};
  bool closed{false};
  std::mutex dispatcher;
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
    if (core->closed || record == core->records.end() || record->second.enqueued) {
      return false;
    }
    record->second.enqueued = true;
    core->ready.push_back(token.value);
  }
  core->ready_changed.notify_one();
  return true;
}

void WeakCompletionMailbox::detach(CompletionToken token) const noexcept {
  const auto core = core_.lock();
  if (!core) {
    return;
  }

  decltype(core->records)::node_type detached;
  {
    std::unique_lock lock{core->mutex};
    const auto record = core->records.find(token.value);
    if (record == core->records.end()) {
      return;
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
  std::unique_lock lock{core->mutex};
  if (core->closed || core->records.contains(token.value)) {
    return false;
  }
  core->records.emplace(token.value, CompletionQueueCore::Record{
                                         .callback = std::move(callback),
                                     });
  return true;
}

bool CompletionQueue::run_one() {
  const auto core = core_;
  std::unique_lock dispatcher{core->dispatcher, std::try_to_lock};
  if (!dispatcher.owns_lock()) {
    return false;
  }

  decltype(core->records)::node_type claimed;
  {
    std::unique_lock lock{core->mutex};
    while (claimed.empty()) {
      core->ready_changed.wait(lock,
                               [&] { return core->closed || !core->ready.empty(); });
      while (!core->ready.empty()) {
        const auto token = core->ready.front();
        core->ready.pop_front();
        const auto record = core->records.find(token);
        if (record != core->records.end()) {
          claimed = core->records.extract(record);
          break;
        }
      }
      if (core->closed) {
        break;
      }
    }
  }
  if (claimed.empty()) {
    return false;
  }

  claimed.mapped().callback();
  return true;
}

std::size_t CompletionQueue::run_ready() {
  const auto core = core_;
  std::unique_lock dispatcher{core->dispatcher, std::try_to_lock};
  if (!dispatcher.owns_lock()) {
    return 0U;
  }

  std::size_t ready_count = 0U;
  {
    std::unique_lock lock{core->mutex};
    ready_count = core->ready.size();
  }

  std::size_t dispatched = 0U;
  for (std::size_t index = 0U; index < ready_count; ++index) {
    decltype(core->records)::node_type claimed;
    {
      std::unique_lock lock{core->mutex};
      if (core->ready.empty()) {
        break;
      }
      const auto token = core->ready.front();
      core->ready.pop_front();
      const auto record = core->records.find(token);
      if (record != core->records.end()) {
        claimed = core->records.extract(record);
      }
    }
    if (claimed.empty()) {
      continue;
    }

    claimed.mapped().callback();
    ++dispatched;
  }
  return dispatched;
}

void CompletionQueue::detach(CompletionToken token) { mailbox().detach(token); }

void CompletionQueue::close() {
  const auto core = core_;
  std::unordered_map<std::uint64_t, CompletionQueueCore::Record> records;
  {
    std::unique_lock lock{core->mutex};
    if (core->closed) {
      return;
    }
    core->closed = true;
    core->ready.clear();
    records.swap(core->records);
  }
  core->ready_changed.notify_all();
}

} // namespace detail
LIBTMUX_NAMESPACE_END
