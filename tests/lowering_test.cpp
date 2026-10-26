#include "libtmux/lowering.hpp"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "libtmux/entities.hpp"
#include "libtmux/relations.hpp"

namespace {

using libtmux::lower;
using libtmux::NodeCollector;
using libtmux::Pane;
namespace pane = libtmux::pane;

struct TextSink {
  std::string out;

  void string_test(std::string_view field, std::string_view op,
                   std::string_view operand) {
    out +=
        std::string{field} + ":" + std::string{op} + "=" + std::string{operand} + " ";
  }
  void number_test(std::string_view field, std::string_view op, long long operand) {
    out += std::string{field} + ":" + std::string{op} + "=" + std::to_string(operand) +
           " ";
  }
  void bool_test(std::string_view field, bool expected) {
    out += std::string{field} + (expected ? ":true " : ":false ");
  }
  void begin_group(bool conjunction) { out += conjunction ? "(and " : "(or "; }
  void end_group() { out += ") "; }
  void begin_negation() { out += "(not "; }
  void end_negation() { out += ") "; }
  void begin_relation(std::string_view name, int quantifier) {
    out += "(" + std::string{name} + ":" + std::to_string(quantifier) + " ";
  }
  void end_relation() { out += ") "; }
};

struct Holder {
  std::vector<Pane> panes;
};

const auto read_panes = [](const Holder& holder) -> const std::vector<Pane>& {
  return holder.panes;
};

TEST(Lowering, EmitsPrefixOrderWithBracketedGroups) {
  const auto expr = pane::command.starts_with("nv") && pane::active;
  TextSink sink;
  lower(expr, sink);
  EXPECT_EQ(sink.out, "(and pane_current_command:startswith=nv pane_active:true ) ");
}

TEST(Lowering, BracketsNegationSoASinkNeverCountsOperands) {
  const auto expr = !pane::active;
  TextSink sink;
  lower(expr, sink);
  EXPECT_EQ(sink.out, "(not pane_active:true ) ");
}

TEST(Lowering, BooleanFieldOperatorsKeepTheirTruthTables) {
  struct Flags {
    bool active;
    bool dead;
  };
  const libtmux::BoolFieldHandle<Flags> active{
      {"active", [](const Flags& row) { return row.active; }}};
  const libtmux::BoolFieldHandle<Flags> dead{
      {"dead", [](const Flags& row) { return row.dead; }}};

  for (const bool left : {false, true}) {
    for (const bool right : {false, true}) {
      const Flags row{left, right};
      EXPECT_EQ((active && dead)(row), left && right);
      EXPECT_EQ((active || dead)(row), left || right);
      EXPECT_EQ((active != false)(row), left);
      EXPECT_EQ((active != true)(row), !left);
    }
  }
}

TEST(Lowering, FieldOperatorsReuseNodesAndOwnStringOperands) {
  const auto rendered = [](const libtmux::FilterExpr<Pane>& expr) {
    TextSink sink;
    lower(expr, sink);
    return sink.out;
  };
  EXPECT_EQ(rendered(pane::active && pane::dead),
            "(and pane_active:true pane_dead:true ) ");
  EXPECT_EQ(rendered(pane::active || pane::dead),
            "(or pane_active:true pane_dead:true ) ");
  EXPECT_EQ(rendered(pane::active != false), "pane_active:true ");
  EXPECT_EQ(rendered(pane::active != true), "pane_active:false ");

  std::string operand = "editor";
  const auto different = pane::title != operand;
  const auto copied = different;
  operand[0] = 'x';
  EXPECT_EQ(rendered(different), "(not pane_title:eq=editor ) ");
  EXPECT_EQ(rendered(copied), "(not pane_title:eq=editor ) ");
}

TEST(Lowering, ARelationCarriesItsChildRatherThanHidingIt) {
  const auto expr =
      libtmux::any_of<Holder>("panes", read_panes, pane::command == "nvim");
  TextSink sink;
  lower(expr, sink);
  // The child compares another entity, and still reaches the sink: this is
  // what a tmux -f compiler would need to translate a relation at all.
  EXPECT_EQ(sink.out, "(panes:0 pane_current_command:eq=nvim ) ");
}

TEST(Lowering, ANestedRelationChildKeepsItsStructure) {
  const auto expr = libtmux::none_of<Holder>(
      "panes", read_panes, pane::command == "nvim" || pane::command == "vi");
  TextSink sink;
  lower(expr, sink);
  EXPECT_EQ(sink.out, "(panes:2 (or pane_current_command:eq=nvim "
                      "pane_current_command:eq=vi ) ) ");
}

TEST(Lowering, CollectingProducesTheSameShapeAsSinking) {
  const auto expr =
      libtmux::any_of<Holder>("panes", read_panes, pane::command == "nvim");
  NodeCollector collector;
  lower(expr, collector);
  ASSERT_EQ(collector.nodes().size(), 3U);
  EXPECT_EQ(collector.nodes()[0].kind, libtmux::LoweredNode::Kind::begin_relation);
  EXPECT_EQ(collector.nodes()[1].kind, libtmux::LoweredNode::Kind::string_test);
  EXPECT_EQ(collector.nodes()[2].kind, libtmux::LoweredNode::Kind::end_relation);
}

TEST(Lowering, ANumericTestKeepsItsValueAsANumber) {
  NodeCollector collector;
  const auto expr = libtmux::pane::width > 80;
  lower(expr, collector);

  ASSERT_EQ(collector.nodes().size(), 1U);
  const auto& node = collector.nodes().front();
  EXPECT_EQ(node.kind, libtmux::LoweredNode::Kind::number_test);
  EXPECT_EQ(node.name, "pane_width");
  EXPECT_EQ(node.op, "gt");
  EXPECT_EQ(node.number, 80);
  // Not rendered into the string operand, which a tmux `-f` compiler would
  // then have to parse back out.
  EXPECT_TRUE(node.operand.empty());
}

TEST(Lowering, EveryNumericOperatorHasItsOwnName) {
  const auto lowered = [](const libtmux::FilterExpr<Pane>& expr) {
    NodeCollector collector;
    lower(expr, collector);
    return collector.nodes().front().op;
  };
  EXPECT_EQ(lowered(libtmux::pane::width == 1), "eq");
  EXPECT_EQ(lowered(libtmux::pane::width != 1), "ne");
  EXPECT_EQ(lowered(libtmux::pane::width < 1), "lt");
  EXPECT_EQ(lowered(libtmux::pane::width <= 1), "lte");
  EXPECT_EQ(lowered(libtmux::pane::width > 1), "gt");
  EXPECT_EQ(lowered(libtmux::pane::width >= 1), "gte");
}

} // namespace
