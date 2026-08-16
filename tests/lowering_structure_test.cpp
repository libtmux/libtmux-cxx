// Structural properties of a lowered expression, whatever anyone serializes
// it into.
//
// This used to restate the schema's kind names in C++ and call that
// conformance. It was not: nothing here read the schema, so the two could
// disagree freely, and they did. The schema is now checked by validating real
// emitted documents against it — `apps/mcp/tests/filter_json_test.cpp` writes
// them and `tools/schema` reads the published file — which leaves this test
// the part that was always its own: a reader of the flat sequence must be able
// to find every operand without counting.
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "libtmux/entities.hpp"
#include "libtmux/lowering.hpp"
#include "libtmux/relations.hpp"

namespace {

using libtmux::lower;
using libtmux::LoweredNode;
using libtmux::NodeCollector;
using libtmux::Pane;
namespace pane = libtmux::pane;

struct Holder {
  std::vector<Pane> panes;
};

TEST(LoweringStructure, BracketsBalanceSoAReaderNeverCountsOperands) {
  const auto read = [](const Holder& holder) -> const std::vector<Pane>& {
    return holder.panes;
  };
  const auto expr = libtmux::any_of<Holder>(
                        "panes", read, !(pane::command == "nvim" && pane::active)) &&
                    libtmux::none_of<Holder>("panes", read, pane::command == "bash");

  NodeCollector collector;
  lower(expr, collector);

  int depth = 0;
  for (const LoweredNode& node : collector.nodes()) {
    switch (node.kind) {
    case LoweredNode::Kind::begin_group:
    case LoweredNode::Kind::begin_negation:
    case LoweredNode::Kind::begin_relation:
      ++depth;
      break;
    case LoweredNode::Kind::end_group:
    case LoweredNode::Kind::end_negation:
    case LoweredNode::Kind::end_relation:
      --depth;
      break;
    default:
      break;
    }
    EXPECT_GE(depth, 0) << "a close preceded its open";
  }
  EXPECT_EQ(depth, 0) << "the sequence did not close every bracket";
}

TEST(LoweringStructure, QuantifiersStayInTheRangeTheWireFormatAllows) {
  const auto read = [](const Holder& holder) -> const std::vector<Pane>& {
    return holder.panes;
  };
  for (const auto& expr : {libtmux::any_of<Holder>("panes", read, pane::active),
                           libtmux::all_of<Holder>("panes", read, pane::active),
                           libtmux::none_of<Holder>("panes", read, pane::active)}) {
    NodeCollector collector;
    lower(expr, collector);
    for (const LoweredNode& node : collector.nodes()) {
      if (node.kind == LoweredNode::Kind::begin_relation) {
        EXPECT_GE(node.quantifier, 0);
        EXPECT_LE(node.quantifier, 3);
      }
    }
  }
}

} // namespace
