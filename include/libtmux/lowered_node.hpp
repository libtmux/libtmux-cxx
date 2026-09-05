#pragma once

// One node of a lowered expression.
//
// Lowering flattens an expression into a sequence a caller can serialize or
// compile without knowing the entity type it came from. That erasure is what
// lets a relation keep its child: the child compares a different entity, so it
// cannot live in the parent's variant, but its lowered form can.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtmux/abi.hpp"

LIBTMUX_NAMESPACE_BEGIN

struct LoweredNode {
  enum class Kind {
    string_test,
    bool_test,
    number_test,
    begin_group,
    end_group,
    begin_negation,
    end_negation,
    begin_relation,
    end_relation,
  };

  Kind kind{Kind::string_test};
  /// Field name for a test, relation name for a relation, empty otherwise.
  std::string name;
  std::string op;
  std::string operand;
  /// The value a number_test compares against, kept as a number so a future
  /// tmux `-f` compiler does not have to parse it back out of the operand.
  long long number{};
  bool conjunction{};
  bool expected{};
  int quantifier{};
};

using LoweredExpression = std::vector<LoweredNode>;

/// Collects a lowered expression. This is the sink the relation builders use to
/// capture their child, and it is a plain value so the result is copyable.
class NodeCollector {
public:
  void string_test(std::string_view field, std::string_view op,
                   std::string_view operand) {
    LoweredNode node;
    node.kind = LoweredNode::Kind::string_test;
    node.name = std::string{field};
    node.op = std::string{op};
    node.operand = std::string{operand};
    nodes_.push_back(std::move(node));
  }
  void bool_test(std::string_view field, bool expected) {
    LoweredNode node;
    node.kind = LoweredNode::Kind::bool_test;
    node.name = std::string{field};
    node.expected = expected;
    nodes_.push_back(std::move(node));
  }
  void number_test(std::string_view field, std::string_view op, long long operand) {
    LoweredNode node;
    node.kind = LoweredNode::Kind::number_test;
    node.name = std::string{field};
    node.op = std::string{op};
    node.number = operand;
    nodes_.push_back(std::move(node));
  }
  void begin_group(bool conjunction) {
    LoweredNode node;
    node.kind = LoweredNode::Kind::begin_group;
    node.conjunction = conjunction;
    nodes_.push_back(std::move(node));
  }
  void end_group() { push(LoweredNode::Kind::end_group); }
  void begin_negation() { push(LoweredNode::Kind::begin_negation); }
  void end_negation() { push(LoweredNode::Kind::end_negation); }
  void begin_relation(std::string_view name, int quantifier) {
    LoweredNode node;
    node.kind = LoweredNode::Kind::begin_relation;
    node.name = std::string{name};
    node.quantifier = quantifier;
    nodes_.push_back(std::move(node));
  }
  void end_relation() { push(LoweredNode::Kind::end_relation); }

  [[nodiscard]] const LoweredExpression& nodes() const noexcept { return nodes_; }
  [[nodiscard]] LoweredExpression take() noexcept { return std::move(nodes_); }

private:
  void push(LoweredNode::Kind kind) {
    LoweredNode node;
    node.kind = kind;
    nodes_.push_back(std::move(node));
  }

  LoweredExpression nodes_;
};

LIBTMUX_NAMESPACE_END
