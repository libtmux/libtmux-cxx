#pragma once

// Value-semantic filter expressions over explicit snapshots.
//
// An expression owns every operand it compares against, so it can outlive the
// call that built it and can be stored, copied, and later translated. It is
// deliberately not an expression template: the node set is a closed variant, so
// the same value that filters a range in memory can be inspected and, later,
// compiled to a tmux `-f` format string.

#include "libtmux/abi.hpp"
#include "libtmux/lowered_node.hpp"
#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

LIBTMUX_NAMESPACE_BEGIN

enum class StringOp { equals, iequals, contains, starts_with, ends_with };
// tmux renders a count, a size and an index as text. Comparing them as text
// puts "9" after "10", so a numeric field is its own kind with its own
// operations rather than a string field a caller must remember to convert.
enum class NumberOp { equals, not_equals, less, less_equal, greater, greater_equal };
enum class Combine { conjunction, disjunction };

// A field is a named accessor: the name is what a future tmux-format lowering
// needs, the accessor is what in-memory evaluation needs.
template <typename Entity> struct StringField {
  std::string_view name;
  std::string_view (*read)(const Entity&);
};

template <typename Entity> struct BoolField {
  std::string_view name;
  bool (*read)(const Entity&);
};

template <typename Entity> struct NumberField {
  std::string_view name;
  long long (*read)(const Entity&);
};

template <typename Entity> class FilterExpr {
public:
  struct StringTest {
    StringField<Entity> field;
    StringOp op;
    std::string operand;
  };

  struct BoolTest {
    BoolField<Entity> field;
    bool expected;
  };

  struct NumberTest {
    NumberField<Entity> field;
    NumberOp op;
    long long operand;
  };

  struct Group {
    Combine combine;
    std::vector<FilterExpr> operands;
  };

  struct Negation {
    std::unique_ptr<FilterExpr> operand;
  };

  // A relation crosses to another entity type, so its child expression cannot
  // live in this variant. The node keeps the relation name and quantifier for
  // inspection and owns the evaluation by value.
  struct RelationTest {
    std::string relation;
    int quantifier;
    std::function<bool(const Entity&)> evaluate;
    // The child compares another entity, so it cannot live in this variant.
    // Its lowered form can, which is what keeps a relation translatable.
    LoweredExpression child;
  };

  using Node =
      std::variant<StringTest, BoolTest, NumberTest, Group, Negation, RelationTest>;

  explicit FilterExpr(Node node) : node_{std::move(node)} {}

  FilterExpr(const FilterExpr& other) : node_{clone(other.node_)} {}
  FilterExpr& operator=(const FilterExpr& other) {
    if (this != &other) {
      node_ = clone(other.node_);
    }
    return *this;
  }
  FilterExpr(FilterExpr&&) noexcept = default;
  FilterExpr& operator=(FilterExpr&&) noexcept = default;
  ~FilterExpr() = default;

  [[nodiscard]] bool operator()(const Entity& entity) const {
    return std::visit([&entity](const auto& node) { return evaluate(node, entity); },
                      node_);
  }

  [[nodiscard]] const Node& node() const noexcept { return node_; }

private:
  static Node clone(const Node& node) {
    return std::visit(
        [](const auto& alternative) -> Node {
          using Alternative = std::decay_t<decltype(alternative)>;
          if constexpr (std::is_same_v<Alternative, Negation>) {
            return Negation{std::make_unique<FilterExpr>(*alternative.operand)};
          } else {
            return alternative;
          }
        },
        node);
  }

  static bool evaluate(const StringTest& test, const Entity& entity) {
    const std::string_view value = test.field.read(entity);
    const std::string_view operand = test.operand;
    switch (test.op) {
    case StringOp::equals:
      return value == operand;
    case StringOp::iequals:
      return value.size() == operand.size() &&
             std::ranges::equal(value, operand, [](char left, char right) {
               return fold(left) == fold(right);
             });
    case StringOp::contains:
      return value.find(operand) != std::string_view::npos;
    case StringOp::starts_with:
      return value.starts_with(operand);
    case StringOp::ends_with:
      return value.ends_with(operand);
    }
    return false;
  }

  static bool evaluate(const BoolTest& test, const Entity& entity) {
    return test.field.read(entity) == test.expected;
  }

  static bool evaluate(const NumberTest& test, const Entity& entity) {
    const long long value = test.field.read(entity);
    switch (test.op) {
    case NumberOp::equals:
      return value == test.operand;
    case NumberOp::not_equals:
      return value != test.operand;
    case NumberOp::less:
      return value < test.operand;
    case NumberOp::less_equal:
      return value <= test.operand;
    case NumberOp::greater:
      return value > test.operand;
    case NumberOp::greater_equal:
      return value >= test.operand;
    }
    return false;
  }

  static bool evaluate(const Group& group, const Entity& entity) {
    // An empty conjunction is true and an empty disjunction is false, so a
    // group folded from no operands never silently matches everything.
    const auto matches = [&entity](const FilterExpr& operand) {
      return operand(entity);
    };
    return group.combine == Combine::conjunction
               ? std::ranges::all_of(group.operands, matches)
               : std::ranges::any_of(group.operands, matches);
  }

  static bool evaluate(const Negation& negation, const Entity& entity) {
    return !(*negation.operand)(entity);
  }

  static bool evaluate(const RelationTest& test, const Entity& entity) {
    return test.evaluate(entity);
  }

  static constexpr char fold(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
  }

  Node node_;
};

// A typed field handle. Only the operations a field's type actually supports
// are declared, so `pane::active.starts_with(...)` is a compile error rather
// than a runtime surprise.
template <typename Entity> struct StringFieldHandle {
  StringField<Entity> field;

  [[nodiscard]] FilterExpr<Entity> operator==(std::string_view operand) const {
    return make(StringOp::equals, operand);
  }
  [[nodiscard]] FilterExpr<Entity> iequals(std::string_view operand) const {
    return make(StringOp::iequals, operand);
  }
  [[nodiscard]] FilterExpr<Entity> contains(std::string_view operand) const {
    return make(StringOp::contains, operand);
  }
  [[nodiscard]] FilterExpr<Entity> starts_with(std::string_view operand) const {
    return make(StringOp::starts_with, operand);
  }
  [[nodiscard]] FilterExpr<Entity> ends_with(std::string_view operand) const {
    return make(StringOp::ends_with, operand);
  }

private:
  [[nodiscard]] FilterExpr<Entity> make(StringOp op, std::string_view operand) const {
    return FilterExpr<Entity>{
        typename FilterExpr<Entity>::StringTest{field, op, std::string{operand}}};
  }
};

template <typename Entity> struct NumberFieldHandle {
  NumberField<Entity> field;

  [[nodiscard]] FilterExpr<Entity> operator==(long long operand) const {
    return make(NumberOp::equals, operand);
  }
  [[nodiscard]] FilterExpr<Entity> operator!=(long long operand) const {
    return make(NumberOp::not_equals, operand);
  }
  [[nodiscard]] FilterExpr<Entity> operator<(long long operand) const {
    return make(NumberOp::less, operand);
  }
  [[nodiscard]] FilterExpr<Entity> operator<=(long long operand) const {
    return make(NumberOp::less_equal, operand);
  }
  [[nodiscard]] FilterExpr<Entity> operator>(long long operand) const {
    return make(NumberOp::greater, operand);
  }
  [[nodiscard]] FilterExpr<Entity> operator>=(long long operand) const {
    return make(NumberOp::greater_equal, operand);
  }

private:
  [[nodiscard]] FilterExpr<Entity> make(NumberOp op, long long operand) const {
    return FilterExpr<Entity>{
        typename FilterExpr<Entity>::NumberTest{field, op, operand}};
  }
};

template <typename Entity> struct BoolFieldHandle {
  BoolField<Entity> field;

  [[nodiscard]] operator FilterExpr<Entity>() const { // NOLINT(*-explicit-*)
    return FilterExpr<Entity>{typename FilterExpr<Entity>::BoolTest{field, true}};
  }
  [[nodiscard]] FilterExpr<Entity> operator==(bool expected) const {
    return FilterExpr<Entity>{typename FilterExpr<Entity>::BoolTest{field, expected}};
  }
};

template <typename Entity>
[[nodiscard]] FilterExpr<Entity> operator&&(FilterExpr<Entity> left,
                                            FilterExpr<Entity> right) {
  std::vector<FilterExpr<Entity>> operands;
  operands.reserve(2);
  operands.push_back(std::move(left));
  operands.push_back(std::move(right));
  return FilterExpr<Entity>{
      typename FilterExpr<Entity>::Group{Combine::conjunction, std::move(operands)}};
}

template <typename Entity>
[[nodiscard]] FilterExpr<Entity> operator||(FilterExpr<Entity> left,
                                            FilterExpr<Entity> right) {
  std::vector<FilterExpr<Entity>> operands;
  operands.reserve(2);
  operands.push_back(std::move(left));
  operands.push_back(std::move(right));
  return FilterExpr<Entity>{
      typename FilterExpr<Entity>::Group{Combine::disjunction, std::move(operands)}};
}

template <typename Entity>
[[nodiscard]] FilterExpr<Entity> operator!(FilterExpr<Entity> operand) {
  return FilterExpr<Entity>{typename FilterExpr<Entity>::Negation{
      std::make_unique<FilterExpr<Entity>>(std::move(operand))}};
}

template <typename Entity>
[[nodiscard]] FilterExpr<Entity> operator&&(FilterExpr<Entity> left,
                                            BoolFieldHandle<Entity> right) {
  return std::move(left) && FilterExpr<Entity>{right};
}

template <typename Entity>
[[nodiscard]] FilterExpr<Entity> operator&&(BoolFieldHandle<Entity> left,
                                            FilterExpr<Entity> right) {
  return FilterExpr<Entity>{left} && std::move(right);
}

template <typename Entity>
[[nodiscard]] FilterExpr<Entity> operator||(FilterExpr<Entity> left,
                                            BoolFieldHandle<Entity> right) {
  return std::move(left) || FilterExpr<Entity>{right};
}

template <typename Entity>
[[nodiscard]] FilterExpr<Entity> operator||(BoolFieldHandle<Entity> left,
                                            FilterExpr<Entity> right) {
  return FilterExpr<Entity>{left} || std::move(right);
}

template <typename Entity>
[[nodiscard]] FilterExpr<Entity> operator!(BoolFieldHandle<Entity> operand) {
  return !FilterExpr<Entity>{operand};
}

// The query vocabulary has a namespace of its own, so a caller composing views
// can say where an adaptor came from without importing the whole library:
//
//   using namespace libtmux::tmuxq;
//   auto interesting = panes | matching(pane::active);
//
// It is a home, not a second surface — `libtmux::matching` below names these
// same functions, so there is one definition to reason about.
namespace tmuxq {

// `matching(expr)` is a range adaptor closure so it composes with std views.
template <typename Entity> [[nodiscard]] auto matching(FilterExpr<Entity> expr) {
  return std::views::filter(
      [expr = std::move(expr)](const Entity& entity) { return expr(entity); });
}

// A bare flag field is a complete question, so it adapts a range directly.
template <typename Entity> [[nodiscard]] auto matching(BoolFieldHandle<Entity> field) {
  return matching(FilterExpr<Entity>{field});
}

} // namespace tmuxq

using tmuxq::matching;

LIBTMUX_NAMESPACE_END
