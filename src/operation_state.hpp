#pragma once

#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

#include "completion_queue.hpp"
#include "libtmux/abi.hpp"
#include "libtmux/command.hpp"

LIBTMUX_NAMESPACE_BEGIN
namespace detail {

template <typename T> using OperationResult = expected<T, CommandFailure>;

template <typename T>
[[nodiscard]] OperationResult<T> move_result_alternative(OperationResult<T>& result) {
  if (!result.has_value()) {
    return unexpected(std::move(result.error()));
  }
  if constexpr (std::is_void_v<T>) {
    return {};
  } else {
    return OperationResult<T>{std::move(*result)};
  }
}

template <typename T>
[[nodiscard]] std::unique_ptr<OperationResult<T>>
box_result_alternative(OperationResult<T>& result) {
  if (!result.has_value()) {
    return std::make_unique<OperationResult<T>>(unexpected(std::move(result.error())));
  }
  if constexpr (std::is_void_v<T>) {
    return std::make_unique<OperationResult<T>>();
  } else {
    return std::make_unique<OperationResult<T>>(std::move(*result));
  }
}

template <typename T>
[[nodiscard]] std::unique_ptr<OperationResult<T>> make_abandoned_outcome() {
  return std::make_unique<OperationResult<T>>(unexpected(CommandFailure{
      .kind = FailureKind::pipe,
      .delivery = DeliveryStatus::not_started,
      .diagnostic = "operation source retired without an outcome",
  }));
}

// tl::expected 1.1.0 marks active-value construction noexcept, so construct the
// callback argument directly from the selected alternative.
template <typename Result, typename T>
struct MoveOnlyFunctionInvoker<Result, OperationResult<T>> final {
  template <typename Callable>
  static Result invoke(Callable& callable, OperationResult<T>& result) {
    auto make_result = [&result] { return move_result_alternative(result); };
    if constexpr (MoveOnlyFunctionReferenceWrapper<Callable>::value) {
      using Target = typename MoveOnlyFunctionReferenceWrapper<Callable>::target_type;
      if constexpr (std::is_member_pointer_v<Target>) {
        return invoke_member(callable.get(), make_result);
      } else {
        return invoke_direct(callable.get(), make_result);
      }
    } else if constexpr (std::is_member_pointer_v<Callable>) {
      return invoke_member(callable, make_result);
    } else {
      return invoke_direct(callable, make_result);
    }
  }

private:
  template <typename Callable, typename Factory>
  static Result invoke_direct(Callable& callable, Factory& make_result) {
    if constexpr (std::is_void_v<Result>) {
      callable(make_result());
    } else {
      return callable(make_result());
    }
  }

  template <typename Callable, typename Factory>
  static Result invoke_member(Callable& callable, Factory& make_result) {
    if constexpr (std::is_void_v<Result>) {
      std::invoke(callable, make_result());
    } else {
      return std::invoke(callable, make_result());
    }
  }
};

template <typename T>
using OperationCallback = MoveOnlyFunction<void(OperationResult<T>)>;

enum class ObserverPhase : std::uint8_t {
  unselected,
  callback,
  blocking,
  blocking_waiting,
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
template <typename T> class OperationCancellation;
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
      : abandoned_outcome_{make_abandoned_outcome<T>()}, hooks_{std::move(hooks)} {
    assert(hooks_);
  }

  [[nodiscard]] bool publish(OperationResult<T>&& result) {
    auto published_outcome = box_result_alternative(result);
    std::unique_ptr<OperationResult<T>> discarded_outcome;
    WeakCompletionMailbox observer_mailbox;
    CompletionToken observer_token;
    bool enqueue_observer = false;
    {
      std::lock_guard lock{mutex_};
      if (outcome_) {
        return false;
      }
      outcome_ = std::move(published_outcome);
      discarded_outcome = std::move(abandoned_outcome_);
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
    return static_cast<bool>(outcome_);
  }

  [[nodiscard]] bool blocking_observer_waiting() const {
    std::lock_guard lock{mutex_};
    return observer_ == ObserverPhase::blocking_waiting;
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
      assert(observer_ == ObserverPhase::blocking);
      observer_ = ObserverPhase::blocking_waiting;
      outcome_changed_.wait(lock, [this] { return static_cast<bool>(outcome_); });
      assert(observer_ == ObserverPhase::blocking_waiting);
      observer_ = ObserverPhase::delivered;
      release = release_hook_locked();
    }
    if (release) {
      release->release_admission();
    }
    return move_result_alternative(*outcome_);
  }

  [[nodiscard]] OperationResult<T> take_callback(CompletionToken token) {
    std::shared_ptr<OperationHooks> release;
    {
      std::unique_lock lock{mutex_};
      assert(outcome_);
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
    return move_result_alternative(*outcome_);
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
    std::unique_ptr<OperationResult<T>> discarded_outcome;
    WeakCompletionMailbox observer_mailbox;
    CompletionToken observer_token;
    bool outcome_published = false;
    bool enqueue_observer = false;
    std::shared_ptr<OperationHooks> release;
    {
      std::lock_guard lock{mutex_};
      const auto abandoned_delivery = transport_ == TransportPhase::queued
                                          ? DeliveryStatus::not_started
                                          : DeliveryStatus::indeterminate;
      transport_ = TransportPhase::retired;
      if (!outcome_) {
        assert(abandoned_outcome_);
        assert(!abandoned_outcome_->has_value());
        abandoned_outcome_->error().delivery = abandoned_delivery;
        outcome_ = std::move(abandoned_outcome_);
        outcome_published = true;
        if (observer_ == ObserverPhase::callback) {
          observer_mailbox = observer_mailbox_;
          observer_token = observer_token_;
          enqueue_observer = true;
        }
      } else {
        discarded_outcome = std::move(abandoned_outcome_);
      }
      release = release_hook_locked();
    }
    if (outcome_published) {
      outcome_changed_.notify_all();
    }
    if (enqueue_observer) {
      static_cast<void>(observer_mailbox.enqueue(observer_token));
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
  std::unique_ptr<OperationResult<T>> outcome_;
  std::unique_ptr<OperationResult<T>> abandoned_outcome_;
  std::shared_ptr<OperationHooks> hooks_;
  CompletionToken observer_token_{};
  WeakCompletionMailbox observer_mailbox_;
  ObserverPhase observer_{ObserverPhase::unselected};
  TransportPhase transport_{TransportPhase::queued};
  bool cancellation_requested_{false};
  bool admission_released_{false};
};

template <typename T> class OperationCancellation final {
public:
  OperationCancellation() noexcept = default;

  [[nodiscard]] bool request_cancel() const {
    const auto state = state_.lock();
    return state && state->request_cancel();
  }

private:
  friend class Operation<T>;
  friend class Subscription<T>;

  explicit OperationCancellation(
      const std::shared_ptr<OperationState<T>>& state) noexcept
      : state_{state} {}

  [[nodiscard]] bool observing(CompletionToken token) const {
    const auto state = state_.lock();
    return state && state->observing_callback(token);
  }

  std::weak_ptr<OperationState<T>> state_;
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

  [[nodiscard]] OperationCancellation<T> cancellation() const noexcept {
    return OperationCancellation<T>{state_};
  }

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
    callback_(move_result_alternative(result));
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
      : cancellation_{std::move(other.cancellation_)},
        mailbox_{std::move(other.mailbox_)},
        token_{std::exchange(other.token_, std::nullopt)} {}

  Subscription& operator=(Subscription&& other) noexcept {
    if (this != &other) {
      detach();
      cancellation_ = std::move(other.cancellation_);
      mailbox_ = std::move(other.mailbox_);
      token_ = std::exchange(other.token_, std::nullopt);
    }
    return *this;
  }

  [[nodiscard]] bool request_cancel() const { return cancellation_.request_cancel(); }

  [[nodiscard]] bool observing() const {
    return token_.has_value() && cancellation_.observing(*token_);
  }

  void detach() noexcept {
    if (token_) {
      mailbox_.detach(*token_);
    }
    cancellation_ = {};
    mailbox_ = {};
    token_.reset();
  }

private:
  friend class Operation<T>;

  Subscription(OperationCancellation<T> cancellation, WeakCompletionMailbox mailbox,
               std::optional<CompletionToken> token) noexcept
      : cancellation_{std::move(cancellation)}, mailbox_{std::move(mailbox)},
        token_{std::move(token)} {}

  OperationCancellation<T> cancellation_;
  WeakCompletionMailbox mailbox_;
  std::optional<CompletionToken> token_;
};

template <typename T>
Subscription<T> Operation<T>::subscribe(CompletionQueue& queue,
                                        OperationCallback<T> callback) && {
  OperationCallback<T> callback_wrapper{std::move(callback)};
  assert(state_);
  const auto token = queue.next_token();
  auto mailbox = queue.mailbox();
  auto cancellation = this->cancellation();
  auto state = std::move(state_);
  state->select_callback(token, mailbox);
  ObserverLease<T> lease{state, token};
  const bool registered = queue.register_record(
      token, ObserverRecord<T>{std::move(lease), std::move(callback_wrapper)});
  if (registered && state->outcome_published()) {
    static_cast<void>(mailbox.enqueue(token));
  }
  return Subscription<T>{std::move(cancellation), std::move(mailbox),
                         registered ? std::optional{token} : std::nullopt};
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

  [[nodiscard]] bool publish(OperationResult<T>&& result) {
    return state_ && state_->publish(std::move(result));
  }

  [[nodiscard]] bool cancel_requested() const {
    return state_ && state_->cancel_requested();
  }

  [[nodiscard]] bool outcome_published() const {
    return state_ && state_->outcome_published();
  }

  [[nodiscard]] bool blocking_observer_waiting() const {
    return state_ && state_->blocking_observer_waiting();
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
