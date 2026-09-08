#pragma once

// Relation quantifiers over to-many and to-one links.
//
// The quantifier is named rather than inferred so a reader never has to guess
// what an empty relation means: `all_of` is satisfied by an empty relation,
// `any_of` is not, and `none_of` is. That is the vacuous-truth convention the
// standard algorithms already use, stated explicitly because getting it wrong
// silently changes which entities a filter returns.

#include "libtmux/abi.hpp"
#include <functional>
#include <ranges>
#include <string>
#include <type_traits>
#include <utility>

#include "libtmux/filter_expr.hpp"
#include "libtmux/lowering.hpp"

LIBTMUX_NAMESPACE_BEGIN

enum class Quantifier { any_of, all_of, none_of, is };

/// Join two listings on the id one of them carries.
///
/// A relation predicate has to reach the related rows without running tmux, so
/// the caller lists both kinds once and links them here. The result borrows the
/// rows it was given, exactly as a filtered view does, so it must not outlive
/// the listing it reads.
template <typename Parent, typename Child>
[[nodiscard]] auto children_of(const std::vector<Child>& rows,
                               StringFieldHandle<Child> foreign_key,
                               StringFieldHandle<Parent> key) {
  return [&rows, foreign_key, key](const Parent& parent) {
    const std::string_view id = key.field.read(parent);
    return rows | std::views::filter([foreign_key, id](const Child& child) {
             return foreign_key.field.read(child) == id;
           });
  };
}

/// The rows are borrowed, so a listing that dies at the semicolon is refused
/// here rather than dangling inside the predicate later.
template <typename Parent, typename Child>
auto children_of(const std::vector<Child>&&, StringFieldHandle<Child>,
                 StringFieldHandle<Parent>) = delete;

/// The same join read the other way: the one row a child points at, or none.
template <typename Child, typename Parent>
[[nodiscard]] auto parent_of(const std::vector<Parent>& rows,
                             StringFieldHandle<Child> foreign_key,
                             StringFieldHandle<Parent> key) {
  return [&rows, foreign_key, key](const Child& child) -> const Parent* {
    const std::string_view id = foreign_key.field.read(child);
    for (const Parent& row : rows) {
      if (key.field.read(row) == id) {
        return &row;
      }
    }
    return nullptr;
  };
}

/// The related entity is whatever the accessor yields, so a bare flag field can
/// stand in for a predicate here exactly as it does in `matching`.
template <typename Entity, typename Read>
using RelatedMany = std::ranges::range_value_t<
    std::remove_cvref_t<std::invoke_result_t<Read, const Entity&>>>;

template <typename Entity, typename Read>
using RelatedOne = std::remove_cvref_t<
    std::remove_pointer_t<std::invoke_result_t<Read, const Entity&>>>;

/// To-many and to-one links take different accessors, so they get different
/// builders rather than one that has to compile both shapes.
template <typename Entity>
[[nodiscard]] FilterExpr<Entity> quantified(std::string name, Quantifier quantifier,
                                            auto read, auto predicate) {
  const FilterExpr<RelatedMany<Entity, decltype(read)>> test{predicate};
  auto evaluate = [quantifier, read, test](const Entity& entity) {
    // `decltype(auto)`, not `const auto&`: an accessor that returns a view by
    // value must stay non-const to be iterated at all, since a filtered view
    // caches its first position. An accessor returning a container reference
    // still binds without copying.
    decltype(auto) related = read(entity);
    const auto matches = [&test](const auto& row) { return test(row); };
    if (quantifier == Quantifier::any_of) {
      return std::ranges::any_of(related, matches);
    }
    if (quantifier == Quantifier::all_of) {
      return std::ranges::all_of(related, matches);
    }
    return std::ranges::none_of(related, matches);
  };
  NodeCollector collector;
  lower(test, collector);
  return FilterExpr<Entity>{typename FilterExpr<Entity>::RelationTest{
      std::move(name), static_cast<int>(quantifier), std::move(evaluate),
      collector.take()}};
}

template <typename Entity>
[[nodiscard]] FilterExpr<Entity> any_of(std::string name, auto read, auto predicate) {
  return quantified<Entity>(std::move(name), Quantifier::any_of, read, predicate);
}

template <typename Entity>
[[nodiscard]] FilterExpr<Entity> all_of(std::string name, auto read, auto predicate) {
  return quantified<Entity>(std::move(name), Quantifier::all_of, read, predicate);
}

template <typename Entity>
[[nodiscard]] FilterExpr<Entity> none_of(std::string name, auto read, auto predicate) {
  return quantified<Entity>(std::move(name), Quantifier::none_of, read, predicate);
}

/// An absent to-one link never satisfies `is`: a window with no active pane is
/// not a window whose active pane runs an editor.
template <typename Entity>
[[nodiscard]] FilterExpr<Entity> is(std::string name, auto read, auto predicate) {
  const FilterExpr<RelatedOne<Entity, decltype(read)>> test{predicate};
  auto evaluate = [read, test](const Entity& entity) {
    const auto* related = read(entity);
    return related != nullptr && test(*related);
  };
  NodeCollector collector;
  lower(test, collector);
  return FilterExpr<Entity>{typename FilterExpr<Entity>::RelationTest{
      std::move(name), static_cast<int>(Quantifier::is), std::move(evaluate),
      collector.take()}};
}

template <typename Child, typename Parent>
auto parent_of(const std::vector<Parent>&&, StringFieldHandle<Child>,
               StringFieldHandle<Parent>) = delete;

LIBTMUX_NAMESPACE_END
