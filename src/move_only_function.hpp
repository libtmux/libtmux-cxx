#pragma once

#include <cassert>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#include "libtmux/abi.hpp"

LIBTMUX_NAMESPACE_BEGIN
namespace detail {

template <typename Signature> class MoveOnlyFunction;

template <typename Result, typename... Args>
class MoveOnlyFunction<Result(Args...)> final {
  class Target {
  public:
    virtual ~Target() = default;
    virtual Result invoke(Args... args) = 0;
  };

  template <typename Callable> class Model final : public Target {
  public:
    explicit Model(Callable callable) : callable_{std::move(callable)} {}

    Result invoke(Args... args) override {
      if constexpr (std::is_void_v<Result>) {
        std::invoke(callable_, std::forward<Args>(args)...);
      } else {
        return std::invoke(callable_, std::forward<Args>(args)...);
      }
    }

  private:
    Callable callable_;
  };

public:
  MoveOnlyFunction() noexcept = default;

  template <typename Callable>
    requires(!std::is_same_v<std::remove_cvref_t<Callable>, MoveOnlyFunction> &&
             std::is_constructible_v<std::decay_t<Callable>, Callable &&> &&
             std::is_invocable_r_v<Result, std::decay_t<Callable>&, Args...>)
  MoveOnlyFunction(Callable&& callable)
      : target_{std::make_unique<Model<std::decay_t<Callable>>>(
            std::forward<Callable>(callable))} {}

  MoveOnlyFunction(const MoveOnlyFunction&) = delete;
  MoveOnlyFunction& operator=(const MoveOnlyFunction&) = delete;
  MoveOnlyFunction(MoveOnlyFunction&&) noexcept = default;
  MoveOnlyFunction& operator=(MoveOnlyFunction&&) noexcept = default;

  explicit operator bool() const noexcept { return static_cast<bool>(target_); }

  Result operator()(Args... args) {
    assert(target_);
    if constexpr (std::is_void_v<Result>) {
      target_->invoke(std::forward<Args>(args)...);
    } else {
      return target_->invoke(std::forward<Args>(args)...);
    }
  }

private:
  std::unique_ptr<Target> target_;
};

} // namespace detail
LIBTMUX_NAMESPACE_END
