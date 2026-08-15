#pragma once

// The one C++23 library facility this package's public surface needs.
//
// Recoverable failure is reported by value, not by exception, which means
// `std::expected`. That type arrived in C++23, so a toolchain shipping a C++20
// standard library cannot provide it. Rather than fork the API, the C++20 build
// substitutes the reference implementation the standard type was modelled on.
//
// Callers write `libtmux::expected` and never name either underlying type, so
// the same source compiles under both.

#if defined(LIBTMUX_USE_TL_EXPECTED)
#include <tl/expected.hpp>
#else
#include <expected>
#endif

#include <type_traits>
#include <utility>

namespace libtmux {

#if defined(LIBTMUX_USE_TL_EXPECTED)

template <typename Value, typename Error> using expected = tl::expected<Value, Error>;

#else

template <typename Value, typename Error> using expected = std::expected<Value, Error>;

#endif

// The unexpected type itself, for the rare declaration that names it.
#if defined(LIBTMUX_USE_TL_EXPECTED)
template <typename Error> using unexpected_t = tl::unexpected<Error>;
#else
template <typename Error> using unexpected_t = std::unexpected<Error>;
#endif

// A factory rather than an alias: an alias template cannot deduce its argument,
// so `unexpected(error)` would stop compiling at every call site.
template <typename Error> [[nodiscard]] constexpr auto unexpected(Error&& error) {
  using Decayed = std::decay_t<Error>;
#if defined(LIBTMUX_USE_TL_EXPECTED)
  return tl::unexpected<Decayed>(std::forward<Error>(error));
#else
  return std::unexpected<Decayed>(std::forward<Error>(error));
#endif
}

} // namespace libtmux
