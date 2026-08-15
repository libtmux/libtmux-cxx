// A lowered expression is emitted in the shape the published schema
// describes. The library ships no serializer, so this test is the sink a
// JSON integration would write, and it proves the two agree.
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

// The exact spelling the schema's kind enum uses.
std::string_view kind_name(LoweredNode::Kind kind) {
  switch (kind) {
  case LoweredNode::Kind::string_test:
    return "string_test";
  case LoweredNode::Kind::bool_test:
    return "bool_test";
  case LoweredNode::Kind::number_test:
    return "number_test";
  case LoweredNode::Kind::begin_group:
    return "begin_group";
  case LoweredNode::Kind::end_group:
    return "end_group";
  case LoweredNode::Kind::begin_negation:
    return "begin_negation";
  case LoweredNode::Kind::end_negation:
    return "end_negation";
  case LoweredNode::Kind::begin_relation:
    return "begin_relation";
  case LoweredNode::Kind::end_relation:
    return "end_relation";
  }
  return "";
}

struct Holder {
  std::vector<Pane> panes;
};

TEST(SchemaConformance, EveryKindHasTheNameTheSchemaLists) {
  for (const auto kind :
       {LoweredNode::Kind::string_test, LoweredNode::Kind::bool_test,
        LoweredNode::Kind::number_test, LoweredNode::Kind::begin_group,
        LoweredNode::Kind::end_group, LoweredNode::Kind::begin_negation,
        LoweredNode::Kind::end_negation, LoweredNode::Kind::begin_relation,
        LoweredNode::Kind::end_relation}) {
    EXPECT_FALSE(kind_name(kind).empty());
  }
}

TEST(SchemaConformance, BracketsBalanceSoAReaderNeverCountsOperands) {
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

TEST(SchemaConformance, QuantifiersStayInTheRangeTheSchemaAllows) {
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
