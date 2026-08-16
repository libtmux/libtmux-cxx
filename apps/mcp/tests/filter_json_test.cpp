// The wire format for a lowered filter expression, and the corpus the
// published schema is checked against.
//
// The old conformance test restated the schema's kind names in C++ and checked
// that brackets balanced. It never loaded the schema, so the schema could say
// anything — and it did: every node required only `kind`, which made
// `{"kind": "string_test"}` a valid document with nothing in it.
//
// This writes real documents to a file. `tools/schema` validates every one of
// them against `schema/filter-expression-v1.schema.json` with a real
// validator, and separately requires a table of malformed documents to be
// rejected. Neither half proves much alone.

#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "libtmux/entities.hpp"
#include "libtmux/lowering.hpp"
#include "libtmux/relations.hpp"
#include "libtmux_consumers/filter_json.hpp"

namespace {

using libtmux::lower;
using libtmux::LoweredExpression;
using libtmux::LoweredNode;
using libtmux::NodeCollector;
using libtmux::Pane;
namespace pane = libtmux::pane;

struct Holder {
  std::vector<Pane> panes;
};

const std::vector<Pane>& panes_of(const Holder& holder) { return holder.panes; }

template <typename Expression> LoweredExpression lowered(const Expression& expr) {
  NodeCollector collector;
  lower(expr, collector);
  return collector.take();
}

// Every node kind and every operator the lowering can produce, so a document
// exercising one of them is in the corpus the schema is checked against.
std::vector<LoweredExpression> corpus() {
  std::vector<LoweredExpression> all;
  all.push_back(lowered(pane::command == "nvim"));
  all.push_back(lowered(pane::command.contains("vi")));
  all.push_back(lowered(pane::command.starts_with("nv")));
  all.push_back(lowered(pane::command.ends_with("im")));
  all.push_back(lowered(pane::command.iequals("NVIM")));
  all.push_back(lowered(libtmux::FilterExpr<Pane>{pane::active}));
  all.push_back(lowered(!pane::active));
  all.push_back(lowered(pane::index == 0));
  all.push_back(lowered(pane::index != 0));
  all.push_back(lowered(pane::index < 4));
  all.push_back(lowered(pane::index <= 4));
  all.push_back(lowered(pane::index > 4));
  all.push_back(lowered(pane::index >= 4));
  all.push_back(lowered(pane::active && pane::command == "zsh"));
  all.push_back(lowered(pane::active || pane::command == "zsh"));
  all.push_back(lowered(libtmux::any_of<Holder>("panes", panes_of, pane::active)));
  all.push_back(lowered(libtmux::all_of<Holder>("panes", panes_of, pane::active)));
  all.push_back(lowered(libtmux::none_of<Holder>("panes", panes_of, pane::active)));
  // Nesting, and an operand carrying the characters that make escaping matter.
  all.push_back(lowered(
      libtmux::any_of<Holder>("panes", panes_of,
                              !(pane::command == "a\"b\\c\n\t␞" && pane::active)) &&
      libtmux::none_of<Holder>("panes", panes_of, pane::command == "bash")));
  return all;
}

TEST(FilterJson, EveryDocumentSurvivesTheRoundTrip) {
  for (const LoweredExpression& expression : corpus()) {
    const nlohmann::json document = libtmux::json_wire::to_json(expression);
    const auto read = libtmux::json_wire::from_json(document);
    ASSERT_TRUE(read.has_value()) << read.error() << " for " << document.dump();
    ASSERT_EQ(read->size(), expression.size());
    for (std::size_t index = 0; index < expression.size(); ++index) {
      const LoweredNode& before = expression[index];
      const LoweredNode& after = (*read)[index];
      EXPECT_EQ(static_cast<int>(after.kind), static_cast<int>(before.kind));
      EXPECT_EQ(after.name, before.name);
      EXPECT_EQ(after.op, before.op);
      EXPECT_EQ(after.operand, before.operand);
      EXPECT_EQ(after.number, before.number);
      EXPECT_EQ(after.conjunction, before.conjunction);
      EXPECT_EQ(after.expected, before.expected);
      EXPECT_EQ(after.quantifier, before.quantifier);
    }
  }
}

TEST(FilterJson, RefusesADocumentItCannotRead) {
  const std::vector<std::string> malformed{
      R"([])",
      R"({"nodes": []})",
      R"({"version": 2, "nodes": []})",
      R"({"version": 1})",
      R"({"version": 1, "nodes": {}})",
      R"({"version": 1, "nodes": [{"kind": "string_test"}]})",
      R"({"version": 1, "nodes": [{"kind": "no_such_kind"}]})",
      R"({"version": 1, "nodes": [{"kind": "bool_test", "name": "pane_active"}]})",
      R"({"version": 1, "nodes": [{"kind": "number_test", "name": "x", "op": "eq"}]})",
      R"({"version": 1, "nodes": [{"kind": "begin_group"}]})",
      R"({"version": 1, "nodes": [{"kind": "begin_relation", "name": "panes",
          "quantifier": 9}]})",
  };
  for (const std::string& text : malformed) {
    const auto document = nlohmann::json::parse(text, nullptr, false);
    ASSERT_FALSE(document.is_discarded()) << text;
    const auto read = libtmux::json_wire::from_json(document);
    EXPECT_FALSE(read.has_value()) << "accepted: " << text;
  }
}

// Written for `tools/schema` to validate. A test that only asserts what this
// file believes is a test of one opinion; the validator reads the published
// schema, which is what a consumer in another language would read.
TEST(FilterJson, WritesTheCorpusForTheSchemaValidator) {
  const char* const destination = std::getenv("LIBTMUX_SCHEMA_CORPUS");
  ASSERT_NE(destination, nullptr)
      << "LIBTMUX_SCHEMA_CORPUS names where to write the corpus";

  std::ofstream out{destination, std::ios::trunc};
  ASSERT_TRUE(out.is_open()) << "cannot write " << destination;
  for (const LoweredExpression& expression : corpus()) {
    out << libtmux::json_wire::to_json(expression).dump() << '\n';
  }
  out.flush();
  ASSERT_TRUE(out.good());
}

} // namespace
