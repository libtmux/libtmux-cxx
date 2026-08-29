#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "libtmux/abi.hpp"
#include "move_only_function.hpp"

LIBTMUX_NAMESPACE_BEGIN
namespace detail {

class CompletionQueueCore;

struct CompletionToken final {
  std::uint64_t value{};
  friend bool operator==(CompletionToken, CompletionToken) = default;
};

class WeakCompletionMailbox final {
public:
  WeakCompletionMailbox() noexcept = default;
  [[nodiscard]] bool enqueue(CompletionToken token) const noexcept;
  void detach(CompletionToken token) const noexcept;

private:
  friend class CompletionQueue;
  explicit WeakCompletionMailbox(std::weak_ptr<CompletionQueueCore> core) noexcept;
  std::weak_ptr<CompletionQueueCore> core_;
};

class CompletionQueue final {
public:
  CompletionQueue();
  ~CompletionQueue();
  CompletionQueue(const CompletionQueue&) = delete;
  CompletionQueue& operator=(const CompletionQueue&) = delete;
  CompletionQueue(CompletionQueue&&) = delete;
  CompletionQueue& operator=(CompletionQueue&&) = delete;

  [[nodiscard]] CompletionToken next_token() noexcept;
  [[nodiscard]] WeakCompletionMailbox mailbox() const noexcept;
  [[nodiscard]] bool register_record(CompletionToken token,
                                     MoveOnlyFunction<void()> callback);
  [[nodiscard]] bool run_one();
  [[nodiscard]] std::size_t run_ready();
  void detach(CompletionToken token);
  void close();

private:
  std::shared_ptr<CompletionQueueCore> core_;
};

} // namespace detail
LIBTMUX_NAMESPACE_END
