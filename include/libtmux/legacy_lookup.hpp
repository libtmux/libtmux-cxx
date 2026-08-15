#pragma once

// Edge parser for the Python `field__lookup=value` spelling.
//
// This exists so a caller migrating from Python libtmux can hand a recorded
// filter string straight across. It is deliberately an edge: it produces the
// same FilterExpr the typed fields produce, so nothing downstream knows a
// string was ever involved, and the typed spelling stays the only way to write
// a new filter.

#include "libtmux/abi.hpp"
#include "libtmux/expected.hpp"
#include <span>
#include <string_view>

#include "libtmux/filter_expr.hpp"

LIBTMUX_NAMESPACE_BEGIN

enum class LookupParseError {
  missing_value,
  unknown_field,
  unknown_lookup,
};

// `eq` is the implicit lookup, matching Python's bare `field=value`. An empty
// name reaches here only from a key that carried no separator at all; a key
// ending in one is refused before this is asked.
[[nodiscard]] inline expected<StringOp, LookupParseError>
lookup_of(std::string_view name) {
  if (name.empty() || name == "eq" || name == "exact") {
    return StringOp::equals;
  }
  if (name == "iexact") {
    return StringOp::iequals;
  }
  if (name == "contains") {
    return StringOp::contains;
  }
  if (name == "startswith") {
    return StringOp::starts_with;
  }
  if (name == "endswith") {
    return StringOp::ends_with;
  }
  return unexpected(LookupParseError::unknown_lookup);
}

// Parse one `field[__lookup]=value` term against a caller-supplied field table.
//
// The value is taken verbatim after the first `=`, so a value containing `=`
// or `__` survives unchanged; only the key is split.
template <typename Entity>
[[nodiscard]] expected<FilterExpr<Entity>, LookupParseError>
parse_lookup(std::string_view term, std::span<const StringFieldHandle<Entity>> fields) {
  const std::size_t assign = term.find('=');
  if (assign == std::string_view::npos) {
    return unexpected(LookupParseError::missing_value);
  }
  const std::string_view key = term.substr(0, assign);
  const std::string_view value = term.substr(assign + 1);

  std::string_view name = key;
  std::string_view lookup;
  if (const std::size_t split = key.rfind("__"); split != std::string_view::npos) {
    name = key.substr(0, split);
    lookup = key.substr(split + 2);
    // A bare field means equality; a field that asked for a lookup and named
    // none did not. Letting `field__=value` through would read as equality,
    // which is a spelling mistake answered with a filter.
    if (lookup.empty()) {
      return unexpected(LookupParseError::unknown_lookup);
    }
  }

  const auto op = lookup_of(lookup);
  if (!op.has_value()) {
    return unexpected(op.error());
  }
  for (const StringFieldHandle<Entity>& field : fields) {
    if (field.field.name == name) {
      switch (*op) {
      case StringOp::equals:
        return field == value;
      case StringOp::iequals:
        return field.iequals(value);
      case StringOp::contains:
        return field.contains(value);
      case StringOp::starts_with:
        return field.starts_with(value);
      case StringOp::ends_with:
        return field.ends_with(value);
      }
    }
  }
  return unexpected(LookupParseError::unknown_field);
}

LIBTMUX_NAMESPACE_END
