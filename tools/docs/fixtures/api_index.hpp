#pragma once

// Focused declarations that protect the API-reference parser's scope rules.

#include <concepts>
#include <string>
#include <utility>

#define FIXTURE_PUBLIC(value) value
#define FIXTURE_CHECK(value) \
  do {                       \
    if (!(value)) {          \
      return;                \
    }                        \
  } while (false)

namespace libtmux {

// A compact enum must not consume the declaration after it.
enum class Mode { direct = 1, queued = 2 };

// A documented enum keeps each caller-facing value's contract.
enum class DocumentedMode : unsigned {
  // Execute immediately.
  immediate,
  // Wait until work is available.
  deferred,
};

// Return a printable mode name.
[[nodiscard]] inline const char* mode_name(Mode mode) {
  const auto body_only = static_cast<int>(mode);
  return body_only == 0 ? "direct" : "queued";
}

// Options are intentionally an aggregate.
struct Options {
  // A label containing comment-looking text.
  std::string label{"https://example.test"};

  // Maximum item count.
  int count{};
};

// A templated public value wrapper.
template <typename T>
class Box final {
 public:
  // Construct a wrapper.
  explicit Box(T value)
      : value_(std::move(value)) {
    const auto constructor_body_only = value_;
    (void)constructor_body_only;
  }

  // Read the wrapped value.
  [[nodiscard]] const T& get() const noexcept { return value_; }

  // The wrapped value type.
  using value_type = T;

 private:
  void hidden();
  T value_;
};

// A template alias must remain a single symbol.
template <typename T>
using BoxAlias = Box<T>;

// A concept may contain a requires-expression body.
template <typename T>
concept Sized = requires(T value) {
  value.size();
};

// A free function template.
template <typename T>
[[nodiscard]] T identity(T value);

// A public constant keeps its initializer identity without its lambda body.
inline constexpr auto transformer{
    [](int value) { return value + 1; }};

namespace named {

// A symbol in a public nested namespace.
inline constexpr int answer = 42;

}  // namespace named

namespace detail {

void hidden_free_function();

}  // namespace detail

#if defined(_WIN32)
// A platform-specific declaration.
inline constexpr bool native_windows = true;
#else
// A platform-specific declaration.
inline constexpr bool native_windows = false;
#endif

}  // namespace libtmux
