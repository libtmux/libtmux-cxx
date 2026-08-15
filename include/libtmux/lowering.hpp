#pragma once

// Walk a FilterExpr and hand its shape to a caller-supplied sink.
//
// The core stays dependency-free by never naming a serialization library: the
// sink is whatever the caller passes, so a JSON integration lives entirely
// outside this header. The same walk is what a future tmux `-f` compiler will
// use, which is why the node keeps its field name rather than only its
// accessor.

#include "libtmux/abi.hpp"
#include <string_view>
#include <variant>

#include "libtmux/filter_expr.hpp"
#include "libtmux/lowered_node.hpp"

LIBTMUX_NAMESPACE_BEGIN

[[nodiscard]] constexpr std::string_view name_of(StringOp op) noexcept {
  switch (op) {
  case StringOp::equals:
    return "eq";
  case StringOp::iequals:
    return "iexact";
  case StringOp::contains:
    return "contains";
  case StringOp::starts_with:
    return "startswith";
  case StringOp::ends_with:
    return "endswith";
  }
  return "eq";
}

[[nodiscard]] constexpr std::string_view name_of(NumberOp op) noexcept {
  switch (op) {
  case NumberOp::equals:
    return "eq";
  case NumberOp::not_equals:
    return "ne";
  case NumberOp::less:
    return "lt";
  case NumberOp::less_equal:
    return "lte";
  case NumberOp::greater:
    return "gt";
  case NumberOp::greater_equal:
    return "gte";
  }
  return "eq";
}

// Replay an already-lowered child into a sink.
template <typename Sink> void replay(const LoweredExpression& nodes, Sink& sink) {
  for (const LoweredNode& node : nodes) {
    switch (node.kind) {
    case LoweredNode::Kind::string_test:
      sink.string_test(node.name, node.op, node.operand);
      break;
    case LoweredNode::Kind::bool_test:
      sink.bool_test(node.name, node.expected);
      break;
    case LoweredNode::Kind::number_test:
      sink.number_test(node.name, node.op, node.number);
      break;
    case LoweredNode::Kind::begin_group:
      sink.begin_group(node.conjunction);
      break;
    case LoweredNode::Kind::end_group:
      sink.end_group();
      break;
    case LoweredNode::Kind::begin_negation:
      sink.begin_negation();
      break;
    case LoweredNode::Kind::end_negation:
      sink.end_negation();
      break;
    case LoweredNode::Kind::begin_relation:
      sink.begin_relation(node.name, node.quantifier);
      break;
    case LoweredNode::Kind::end_relation:
      sink.end_relation();
      break;
    }
  }
}

// A sink receives one call per node in prefix order. Groups and negations are
// bracketed by begin/end so a sink never has to count operands itself.
template <typename Entity, typename Sink>
void lower(const FilterExpr<Entity>& expr, Sink& sink) {
  using Expr = FilterExpr<Entity>;
  std::visit(
      [&sink](const auto& node) {
        using Node = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<Node, typename Expr::StringTest>) {
          sink.string_test(node.field.name, name_of(node.op), node.operand);
        } else if constexpr (std::is_same_v<Node, typename Expr::BoolTest>) {
          sink.bool_test(node.field.name, node.expected);
        } else if constexpr (std::is_same_v<Node, typename Expr::NumberTest>) {
          sink.number_test(node.field.name, name_of(node.op), node.operand);
        } else if constexpr (std::is_same_v<Node, typename Expr::Group>) {
          sink.begin_group(node.combine == Combine::conjunction);
          for (const Expr& operand : node.operands) {
            lower(operand, sink);
          }
          sink.end_group();
        } else if constexpr (std::is_same_v<Node, typename Expr::Negation>) {
          sink.begin_negation();
          lower(*node.operand, sink);
          sink.end_negation();
        } else {
          // The child was lowered when the relation was built, so it replays
          // here between markers and a sink sees one complete expression.
          sink.begin_relation(node.relation, node.quantifier);
          replay(node.child, sink);
          sink.end_relation();
        }
      },
      expr.node());
}

LIBTMUX_NAMESPACE_END
