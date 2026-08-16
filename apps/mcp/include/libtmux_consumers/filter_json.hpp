#pragma once

// The lowered filter expression as JSON, in the shape `schema/` publishes.
//
// Here rather than in the library because the library encodes nothing: the
// core has no JSON dependency and is not getting one. This is the opt-in
// integration that turns a `LoweredExpression` into the wire format, and it is
// the only implementation of that format — a second one is how a schema and
// its documents drift apart.
//
// Reading is offered alongside writing because a format with no reader is a
// format nobody has checked. Round-tripping is what the test asserts, and it
// is what a `-f` compiler or another language binding will need.

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "libtmux/expected.hpp"
#include "libtmux/lowered_node.hpp"

namespace libtmux::json_wire {

inline constexpr int kSchemaVersion = 1;

// Every kind, in the spelling the schema's enum uses. One table, so the writer
// and the reader cannot disagree about a name.
[[nodiscard]] inline std::string_view name_of(LoweredNode::Kind kind) noexcept {
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
  return {};
}

[[nodiscard]] inline libtmux::expected<LoweredNode::Kind, std::string>
kind_from(std::string_view name) {
  for (const auto kind :
       {LoweredNode::Kind::string_test, LoweredNode::Kind::bool_test,
        LoweredNode::Kind::number_test, LoweredNode::Kind::begin_group,
        LoweredNode::Kind::end_group, LoweredNode::Kind::begin_negation,
        LoweredNode::Kind::end_negation, LoweredNode::Kind::begin_relation,
        LoweredNode::Kind::end_relation}) {
    if (name_of(kind) == name) {
      return kind;
    }
  }
  return libtmux::unexpected("no such node kind: " + std::string{name});
}

// A node carries only the members its kind gives meaning to. The schema closes
// each kind with `additionalProperties: false`, so writing the unused ones
// would produce documents the schema rejects — which is the point of writing
// it that way: a reader never has to wonder whether an absent field was
// omitted or meant.
[[nodiscard]] inline nlohmann::json to_json(const LoweredNode& node) {
  nlohmann::json out{{"kind", name_of(node.kind)}};
  switch (node.kind) {
  case LoweredNode::Kind::string_test:
    out["name"] = node.name;
    out["op"] = node.op;
    out["operand"] = node.operand;
    break;
  case LoweredNode::Kind::bool_test:
    out["name"] = node.name;
    out["expected"] = node.expected;
    break;
  case LoweredNode::Kind::number_test:
    out["name"] = node.name;
    out["op"] = node.op;
    out["number"] = node.number;
    break;
  case LoweredNode::Kind::begin_group:
    out["conjunction"] = node.conjunction;
    break;
  case LoweredNode::Kind::begin_relation:
    out["name"] = node.name;
    out["quantifier"] = node.quantifier;
    break;
  case LoweredNode::Kind::end_group:
  case LoweredNode::Kind::end_negation:
  case LoweredNode::Kind::end_relation:
  case LoweredNode::Kind::begin_negation:
    break;
  }
  return out;
}

[[nodiscard]] inline nlohmann::json to_json(const LoweredExpression& nodes) {
  nlohmann::json listed = nlohmann::json::array();
  for (const LoweredNode& node : nodes) {
    listed.push_back(to_json(node));
  }
  return nlohmann::json{{"version", kSchemaVersion}, {"nodes", std::move(listed)}};
}

// Refuses anything it cannot read, rather than filling in a default. A field
// that was absent because the sender meant nothing by it and one that was
// absent because the sender is a different version are the same bytes, and
// guessing between them is how a filter silently stops filtering.
[[nodiscard]] inline libtmux::expected<LoweredExpression, std::string>
from_json(const nlohmann::json& document) {
  if (!document.is_object()) {
    return libtmux::unexpected(std::string{"a document must be an object"});
  }
  const auto version = document.find("version");
  if (version == document.end() || !version->is_number_integer() ||
      version->get<int>() != kSchemaVersion) {
    return libtmux::unexpected(std::string{"unsupported or missing schema version"});
  }
  const auto listed = document.find("nodes");
  if (listed == document.end() || !listed->is_array()) {
    return libtmux::unexpected(std::string{"nodes must be an array"});
  }

  const auto text = [](const nlohmann::json& node, const char* key)
      -> libtmux::expected<std::string, std::string> {
    const auto found = node.find(key);
    if (found == node.end() || !found->is_string()) {
      return libtmux::unexpected(std::string{key} + " must be a string");
    }
    return found->get<std::string>();
  };

  LoweredExpression nodes;
  for (const nlohmann::json& entry : *listed) {
    if (!entry.is_object()) {
      return libtmux::unexpected(std::string{"a node must be an object"});
    }
    auto named = text(entry, "kind");
    if (!named.has_value()) {
      return libtmux::unexpected(named.error());
    }
    auto kind = kind_from(*named);
    if (!kind.has_value()) {
      return libtmux::unexpected(kind.error());
    }

    LoweredNode node;
    node.kind = *kind;
    switch (node.kind) {
    case LoweredNode::Kind::string_test: {
      auto name = text(entry, "name");
      auto op = text(entry, "op");
      auto operand = text(entry, "operand");
      if (!name.has_value() || !op.has_value() || !operand.has_value()) {
        return libtmux::unexpected(std::string{"a string_test needs name, op, operand"});
      }
      node.name = *std::move(name);
      node.op = *std::move(op);
      node.operand = *std::move(operand);
      break;
    }
    case LoweredNode::Kind::bool_test: {
      auto name = text(entry, "name");
      const auto expected = entry.find("expected");
      if (!name.has_value() || expected == entry.end() || !expected->is_boolean()) {
        return libtmux::unexpected(std::string{"a bool_test needs name and expected"});
      }
      node.name = *std::move(name);
      node.expected = expected->get<bool>();
      break;
    }
    case LoweredNode::Kind::number_test: {
      auto name = text(entry, "name");
      auto op = text(entry, "op");
      const auto number = entry.find("number");
      if (!name.has_value() || !op.has_value() || number == entry.end() ||
          !number->is_number_integer()) {
        return libtmux::unexpected(std::string{"a number_test needs name, op, number"});
      }
      node.name = *std::move(name);
      node.op = *std::move(op);
      node.number = number->get<long long>();
      break;
    }
    case LoweredNode::Kind::begin_group: {
      const auto conjunction = entry.find("conjunction");
      if (conjunction == entry.end() || !conjunction->is_boolean()) {
        return libtmux::unexpected(std::string{"a begin_group needs conjunction"});
      }
      node.conjunction = conjunction->get<bool>();
      break;
    }
    case LoweredNode::Kind::begin_relation: {
      auto name = text(entry, "name");
      const auto quantifier = entry.find("quantifier");
      if (!name.has_value() || quantifier == entry.end() ||
          !quantifier->is_number_integer()) {
        return libtmux::unexpected(
            std::string{"a begin_relation needs name and quantifier"});
      }
      const auto value = quantifier->get<int>();
      if (value < 0 || value > 3) {
        return libtmux::unexpected(std::string{"quantifier out of range"});
      }
      node.name = *std::move(name);
      node.quantifier = value;
      break;
    }
    case LoweredNode::Kind::end_group:
    case LoweredNode::Kind::end_negation:
    case LoweredNode::Kind::end_relation:
    case LoweredNode::Kind::begin_negation:
      break;
    }
    nodes.push_back(std::move(node));
  }
  return nodes;
}

} // namespace libtmux::json_wire
