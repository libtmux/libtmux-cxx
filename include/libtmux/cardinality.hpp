#pragma once

// Exception-free cardinality over snapshot views.
//
// Callers ask for one entity far more often than they want to handle a range,
// and the two ways that request can fail are not the same failure: finding
// nothing is ordinary, finding several means the caller's filter was wrong.
// These return types keep both outcomes in the value channel.

#include "libtmux/abi.hpp"
#include "libtmux/expected.hpp"
#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string_view>
#include <type_traits>

LIBTMUX_NAMESPACE_BEGIN

/// Why a lookup that required exactly one match did not get one.
///
/// The two want opposite fixes, which is why they are distinct: one needs
/// wider criteria and the other needs narrower.
enum class CardinalityError { none_matched, several_matched };

[[nodiscard]] constexpr std::string_view to_string(CardinalityError error) noexcept {
  switch (error) {
  case CardinalityError::none_matched:
    return "nothing matched";
  case CardinalityError::several_matched:
    return "several matched where one was required";
  }
  return "unknown cardinality error";
}

template <std::ranges::input_range Range>
/// A borrowed element of a range, rather than a copy of it.
///
/// These lookups reference an element of the range they were given, so the
/// range has to outlive the result. That is why they take an lvalue.
using Referenced = std::reference_wrapper<const std::ranges::range_value_t<Range>>;

// Both take an lvalue on purpose. The result references an element of the
// range, so binding a temporary here — a pipeline over an owning view, or a
// list returned by value — leaves that reference dangling at the semicolon.
// Requiring a name makes the storage the caller must keep alive visible.

/// Both also require the range to yield references. A range whose elements are
/// produced on demand — a transform that returns by value, say — has nothing
/// for the answer to refer to, and the temporary dies with the call.
template <typename Range>
concept ReferenceRange =
    std::ranges::input_range<Range> &&
    std::is_lvalue_reference_v<std::ranges::range_reference_t<Range>>;

/// `first` states that a caller tolerates extras; it never reports several.
template <ReferenceRange Range>
[[nodiscard]] std::optional<Referenced<Range>> first(Range& range) {
  auto iterator = std::ranges::begin(range);
  if (iterator == std::ranges::end(range)) {
    return std::nullopt;
  }
  return std::cref(*iterator);
}

/// `exactly_one` states that several is a caller error, and says which one.
template <ReferenceRange Range>
[[nodiscard]] expected<Referenced<Range>, CardinalityError> exactly_one(Range& range) {
  auto iterator = std::ranges::begin(range);
  const auto last = std::ranges::end(range);
  if (iterator == last) {
    return unexpected(CardinalityError::none_matched);
  }
  // A pointer, not a reference: binding `const auto&` to `*iterator` would
  // extend the lifetime of anything the range produced on demand, and then
  // return a reference to it. The constraint rules that range out; taking the
  // address says so at the one line where it would have mattered.
  const auto* only = std::addressof(*iterator);
  if (++iterator != last) {
    return unexpected(CardinalityError::several_matched);
  }
  return std::cref(*only);
}

/// Copies referenced elements; moves when an iterator yields an rvalue. A
// temporary view over an lvalue container therefore leaves that container intact.
// Owning the element does not extend storage borrowed by its own members.
template <std::ranges::input_range Range>
  requires std::constructible_from<std::ranges::range_value_t<Range>,
                                   std::ranges::range_reference_t<Range>>
[[nodiscard]] std::optional<std::ranges::range_value_t<Range>>
first_owned(Range&& range) {
  auto iterator = std::ranges::begin(range);
  if (iterator == std::ranges::end(range)) {
    return std::nullopt;
  }
  return std::optional<std::ranges::range_value_t<Range>>{std::in_place, *iterator};
}

/// A forward range's iterator can be copied and advanced independently of the
// original, so a second element rules the range out before the first is
// materialized. A single-pass range (a stream, a generator) shares mutable
// state between copies instead, so it has no way to look ahead: the first
// element must be materialized before the range can be advanced to check for
// a second, and an error there still consumes it.
template <std::ranges::input_range Range>
  requires std::constructible_from<std::ranges::range_value_t<Range>,
                                   std::ranges::range_reference_t<Range>> &&
           std::move_constructible<std::ranges::range_value_t<Range>>
[[nodiscard]] expected<std::ranges::range_value_t<Range>, CardinalityError>
exactly_one_owned(Range&& range) {
  auto iterator = std::ranges::begin(range);
  const auto last = std::ranges::end(range);
  if (iterator == last) {
    return unexpected(CardinalityError::none_matched);
  }
  if constexpr (std::ranges::forward_range<Range>) {
    auto second = iterator;
    if (++second != last) {
      return unexpected(CardinalityError::several_matched);
    }
    return std::ranges::range_value_t<Range>(*iterator);
  } else {
    std::ranges::range_value_t<Range> only(*iterator);
    if (++iterator != last) {
      return unexpected(CardinalityError::several_matched);
    }
    return only;
  }
}

LIBTMUX_NAMESPACE_END
