#pragma once

// The one C++23 library facility this package's public surface needs.
//
// Recoverable failure is reported by value, not by exception, which means
// `std::expected`. Callers write `libtmux::expected` and never name the
// standard type, so a later change of representation stays source compatible.

#include <expected>
// libstdc++ defines `std::expected` only when the compiler advertises the
// concepts support its header is written against, which clang before 19 does
// not when it drives libstdc++. Without this the first thing a builder sees is
// a template error inside this header, which names neither the toolchain nor
// the way out.
#if !defined(__cpp_lib_expected)
#error                                                                                 \
    "libtmux needs std::expected, and this standard library does not provide it. Build with libc++ (-stdlib=libc++), with GCC 13 or newer, or with clang 19 or newer."
#endif

#include <type_traits>
#include <utility>

namespace libtmux {

/// A value or the reason there is not one. An alias of `std::expected`.
template <typename Value, typename Error> using expected = std::expected<Value, Error>;

/// The error side of `expected`, named for the rare declaration that has to
/// spell it. Returning one is how a function reports a failure.
template <typename Error> using unexpected_t = std::unexpected<Error>;

/// What `value()` throws when there is none.
template <typename Error> using bad_expected_access = std::bad_expected_access<Error>;

/// A factory rather than an alias: an alias template cannot deduce its argument,
/// so `unexpected(error)` would stop compiling at every call site.
template <typename Error> [[nodiscard]] constexpr auto unexpected(Error&& error) {
  return std::unexpected<std::decay_t<Error>>(std::forward<Error>(error));
}

} // namespace libtmux
