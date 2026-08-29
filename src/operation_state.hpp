#pragma once

#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "libtmux/abi.hpp"
#include "libtmux/command.hpp"

LIBTMUX_NAMESPACE_BEGIN
namespace detail {

template <typename T> using OperationResult = expected<T, CommandFailure>;

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
    {
      std::lock_guard lock{mutex_};
      if (outcome_) {
        return false;
      }
      outcome_.emplace(std::move(result));
    }
    outcome_changed_.notify_all();
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
