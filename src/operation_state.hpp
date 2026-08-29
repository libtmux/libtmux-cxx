#pragma once

#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "completion_queue.hpp"
#include "libtmux/abi.hpp"
#include "libtmux/command.hpp"

LIBTMUX_NAMESPACE_BEGIN
namespace detail {

template <typename T> using OperationResult = expected<T, CommandFailure>;

template <typename T>
using OperationCallback = MoveOnlyFunction<void(OperationResult<T>)>;

enum class ObserverPhase : std::uint8_t {
  unselected,
  callback,
  blocking,
  detached,
  delivered,
};

enum class TransportPhase : std::uint8_t {
  queued,
  dispatching,
  active,
  retiring,
  retired,
};

class OperationHooks {
public:
  virtual ~OperationHooks() = default;
  virtual void wake_reactor() noexcept = 0;
  virtual void release_admission() noexcept = 0;
};

template <typename T> class OperationState;
template <typename T> class Operation;
template <typename T> class OperationSource;
template <typename T> class Subscription;

template <typename T> struct StartedOperation final {
  Operation<T> operation;
  OperationSource<T> source;
};

template <typename T>
[[nodiscard]] StartedOperation<T> make_operation(std::shared_ptr<OperationHooks> hooks);

template <typename T>
[[nodiscard]] OperationResult<T> sync_wait(Operation<T>&& operation);

template <typename T> class OperationState final {
public:
  explicit OperationState(std::shared_ptr<OperationHooks> hooks)
      : hooks_{std::move(hooks)} {
    assert(hooks_);
  }

  [[nodiscard]] bool publish(OperationResult<T> result) {
    WeakCompletionMailbox observer_mailbox;
    CompletionToken observer_token;
    bool enqueue_observer = false;
    {
      std::lock_guard lock{mutex_};
      if (outcome_) {
        return false;
      }
      outcome_.emplace(std::move(result));
      if (observer_ == ObserverPhase::callback) {
        observer_mailbox = observer_mailbox_;
        observer_token = observer_token_;
        enqueue_observer = true;
      }
    }
    outcome_changed_.notify_all();
    if (enqueue_observer) {
      static_cast<void>(observer_mailbox.enqueue(observer_token));
    }
    return true;
  }

  [[nodiscard]] bool request_cancel() {
    std::shared_ptr<OperationHooks> hooks;
    {
      std::lock_guard lock{mutex_};
      if (cancellation_requested_ || outcome_ ||
          transport_ == TransportPhase::retired) {
        return false;
      }
      cancellation_requested_ = true;
      hooks = hooks_;
    }
    hooks->wake_reactor();
    return true;
  }

  [[nodiscard]] bool cancel_requested() const {
    std::lock_guard lock{mutex_};
    return cancellation_requested_;
  }

  [[nodiscard]] bool outcome_published() const {
    std::lock_guard lock{mutex_};
    return outcome_.has_value();
  }

  void select_blocking() {
    std::lock_guard lock{mutex_};
    assert(observer_ == ObserverPhase::unselected);
    observer_ = ObserverPhase::blocking;
  }

  void select_callback(CompletionToken token, WeakCompletionMailbox mailbox) {
    std::lock_guard lock{mutex_};
    assert(observer_ == ObserverPhase::unselected);
    observer_ = ObserverPhase::callback;
    observer_token_ = token;
    observer_mailbox_ = std::move(mailbox);
  }

  [[nodiscard]] OperationResult<T> wait_and_take() {
    std::shared_ptr<OperationHooks> release;
    {
      std::unique_lock lock{mutex_};
      outcome_changed_.wait(lock, [this] { return outcome_.has_value(); });
      assert(observer_ == ObserverPhase::blocking);
      observer_ = ObserverPhase::delivered;
      release = release_hook_locked();
    }
    if (release) {
      release->release_admission();
    }
    return std::move(*outcome_);
  }

  [[nodiscard]] OperationResult<T> take_callback(CompletionToken token) {
    std::shared_ptr<OperationHooks> release;
    {
      std::unique_lock lock{mutex_};
      assert(outcome_.has_value());
      assert(observer_ == ObserverPhase::callback);
      assert(observer_token_ == token);
      observer_ = ObserverPhase::delivered;
      observer_token_ = {};
      observer_mailbox_ = {};
      release = release_hook_locked();
    }
    if (release) {
      release->release_admission();
    }
    return std::move(*outcome_);
  }

  [[nodiscard]] bool observing_callback(CompletionToken token) const {
    std::lock_guard lock{mutex_};
    return observer_ == ObserverPhase::callback && observer_token_ == token;
  }

  void detach_callback(CompletionToken token) noexcept {
    std::shared_ptr<OperationHooks> release;
    {
      std::lock_guard lock{mutex_};
      if (observer_ != ObserverPhase::callback || observer_token_ != token) {
        return;
      }
      observer_ = ObserverPhase::detached;
      observer_token_ = {};
      observer_mailbox_ = {};
      release = release_hook_locked();
    }
    if (release) {
      release->release_admission();
    }
  }

  void detach_unselected() noexcept {
    std::shared_ptr<OperationHooks> release;
    {
      std::lock_guard lock{mutex_};
      if (observer_ != ObserverPhase::unselected) {
        return;
      }
      observer_ = ObserverPhase::detached;
      release = release_hook_locked();
    }
    if (release) {
      release->release_admission();
    }
  }

  void mark_dispatching() { advance_transport(TransportPhase::dispatching); }
  void mark_active() { advance_transport(TransportPhase::active); }
  void begin_retirement() { advance_transport(TransportPhase::retiring); }

  void retire() noexcept {
    std::shared_ptr<OperationHooks> release;
    {
      std::lock_guard lock{mutex_};
      transport_ = TransportPhase::retired;
      release = release_hook_locked();
    }
    if (release) {
      release->release_admission();
    }
  }

private:
  void advance_transport(TransportPhase phase) {
    std::lock_guard lock{mutex_};
    assert(transport_ <= phase);
    transport_ = phase;
  }

  [[nodiscard]] std::shared_ptr<OperationHooks> release_hook_locked() {
    const bool observer_finished =
        observer_ == ObserverPhase::detached || observer_ == ObserverPhase::delivered;
    if (admission_released_ || transport_ != TransportPhase::retired ||
        !observer_finished) {
      return {};
    }
    admission_released_ = true;
    return std::move(hooks_);
  }

  mutable std::mutex mutex_;
  std::condition_variable outcome_changed_;
  std::optional<OperationResult<T>> outcome_;
  std::shared_ptr<OperationHooks> hooks_;
  CompletionToken observer_token_{};
  WeakCompletionMailbox observer_mailbox_;
  ObserverPhase observer_{ObserverPhase::unselected};
  TransportPhase transport_{TransportPhase::queued};
  bool cancellation_requested_{false};
  bool admission_released_{false};
};

template <typename T> class Operation final {
public:
  Operation() noexcept = default;
  ~Operation() { reset(); }
  Operation(const Operation&) = delete;
  Operation& operator=(const Operation&) = delete;

  Operation(Operation&& other) noexcept : state_{std::move(other.state_)} {}

  Operation& operator=(Operation&& other) noexcept {
    if (this != &other) {
      reset();
      state_ = std::move(other.state_);
    }
    return *this;
  }

  [[nodiscard]] bool request_cancel() { return state_ && state_->request_cancel(); }

  [[nodiscard]] Subscription<T> subscribe(CompletionQueue& queue,
                                          OperationCallback<T> callback) &&;

  Subscription<T> subscribe(CompletionQueue&, OperationCallback<T>) & = delete;

private:
  template <typename U>
  friend StartedOperation<U> make_operation(std::shared_ptr<OperationHooks> hooks);
  template <typename U> friend OperationResult<U> sync_wait(Operation<U>&& operation);

  explicit Operation(std::shared_ptr<OperationState<T>> state) noexcept
      : state_{std::move(state)} {}

  void reset() noexcept {
    if (state_) {
      state_->detach_unselected();
      state_.reset();
    }
  }

  std::shared_ptr<OperationState<T>> state_;
};

template <typename T> class ObserverLease final {
public:
  ObserverLease(std::shared_ptr<OperationState<T>> state,
                CompletionToken token) noexcept
      : state_{std::move(state)}, token_{token} {}

  ~ObserverLease() {
    if (state_) {
      state_->detach_callback(token_);
    }
  }

  ObserverLease(const ObserverLease&) = delete;
  ObserverLease& operator=(const ObserverLease&) = delete;
  ObserverLease(ObserverLease&&) noexcept = default;
  ObserverLease& operator=(ObserverLease&&) = delete;

  [[nodiscard]] OperationResult<T> take() {
    auto state = std::move(state_);
    return state->take_callback(token_);
  }

private:
  std::shared_ptr<OperationState<T>> state_;
  CompletionToken token_{};
};

template <typename T> class ObserverRecord final {
public:
  ObserverRecord(ObserverLease<T> lease, OperationCallback<T> callback) noexcept
      : lease_{std::move(lease)}, callback_{std::move(callback)} {}

  ObserverRecord(const ObserverRecord&) = delete;
  ObserverRecord& operator=(const ObserverRecord&) = delete;
  ObserverRecord(ObserverRecord&&) noexcept = default;
  ObserverRecord& operator=(ObserverRecord&&) = delete;

  void operator()() {
    auto result = lease_.take();
    callback_(std::move(result));
  }

private:
  ObserverLease<T> lease_;
  OperationCallback<T> callback_;
};

template <typename T> class Subscription final {
public:
  Subscription() noexcept = default;
  ~Subscription() { detach(); }
  Subscription(const Subscription&) = delete;
  Subscription& operator=(const Subscription&) = delete;

  Subscription(Subscription&& other) noexcept
      : state_{std::move(other.state_)}, mailbox_{std::move(other.mailbox_)},
        token_{std::exchange(other.token_, {})} {}

  Subscription& operator=(Subscription&& other) noexcept {
    if (this != &other) {
      detach();
      state_ = std::move(other.state_);
      mailbox_ = std::move(other.mailbox_);
      token_ = std::exchange(other.token_, {});
    }
    return *this;
  }

  [[nodiscard]] bool request_cancel() { return state_ && state_->request_cancel(); }

  [[nodiscard]] bool observing() const {
    return state_ && state_->observing_callback(token_);
  }

  void detach() noexcept {
    if (state_) {
      mailbox_.detach(token_);
      state_.reset();
      mailbox_ = {};
      token_ = {};
    }
  }

private:
  friend class Operation<T>;

  Subscription(std::shared_ptr<OperationState<T>> state, WeakCompletionMailbox mailbox,
               CompletionToken token) noexcept
      : state_{std::move(state)}, mailbox_{std::move(mailbox)}, token_{token} {}

  std::shared_ptr<OperationState<T>> state_;
  WeakCompletionMailbox mailbox_;
  CompletionToken token_{};
};

template <typename T>
Subscription<T> Operation<T>::subscribe(CompletionQueue& queue,
                                        OperationCallback<T> callback) && {
  OperationCallback<T> callback_wrapper{std::move(callback)};
  assert(state_);
  const auto token = queue.next_token();
  auto mailbox = queue.mailbox();
  auto state = std::move(state_);
  state->select_callback(token, mailbox);
  ObserverLease<T> lease{state, token};
  const bool registered = queue.register_record(
      token, ObserverRecord<T>{std::move(lease), std::move(callback_wrapper)});
  if (registered && state->outcome_published()) {
    static_cast<void>(mailbox.enqueue(token));
  }
  return Subscription<T>{std::move(state), std::move(mailbox), token};
}

template <typename T> class OperationSource final {
public:
  OperationSource() noexcept = default;
  ~OperationSource() { retire(); }
  OperationSource(const OperationSource&) = delete;
  OperationSource& operator=(const OperationSource&) = delete;

  OperationSource(OperationSource&& other) noexcept : state_{std::move(other.state_)} {}

  OperationSource& operator=(OperationSource&& other) noexcept {
    if (this != &other) {
      retire();
      state_ = std::move(other.state_);
    }
    return *this;
  }

  [[nodiscard]] bool publish(OperationResult<T> result) {
    return state_ && state_->publish(std::move(result));
  }

  [[nodiscard]] bool cancel_requested() const {
    return state_ && state_->cancel_requested();
  }

  [[nodiscard]] bool outcome_published() const {
    return state_ && state_->outcome_published();
  }

  void mark_dispatching() {
    if (state_) {
      state_->mark_dispatching();
    }
  }

  void mark_active() {
    if (state_) {
      state_->mark_active();
    }
  }

  void begin_retirement() {
    if (state_) {
      state_->begin_retirement();
    }
  }

  void retire() noexcept {
    if (state_) {
      state_->retire();
    }
  }

private:
  template <typename U>
  friend StartedOperation<U> make_operation(std::shared_ptr<OperationHooks> hooks);

  explicit OperationSource(std::shared_ptr<OperationState<T>> state) noexcept
      : state_{std::move(state)} {}

  std::shared_ptr<OperationState<T>> state_;
};

template <typename T>
[[nodiscard]] StartedOperation<T>
make_operation(std::shared_ptr<OperationHooks> hooks) {
  auto state = std::make_shared<OperationState<T>>(std::move(hooks));
  return StartedOperation<T>{Operation<T>{state}, OperationSource<T>{std::move(state)}};
}

template <typename T>
[[nodiscard]] OperationResult<T> sync_wait(Operation<T>&& operation) {
  assert(operation.state_);
  auto state = std::move(operation.state_);
  state->select_blocking();
  return state->wait_and_take();
}

} // namespace detail
LIBTMUX_NAMESPACE_END
