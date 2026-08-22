#include "schema.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "libtmux/version.hpp"

namespace libtmux::mcp::server {
namespace {

#if defined(_WIN32)
constexpr std::string_view kInstructions =
    "Start with inspect_tmux. On psmux, keep the session ID with every window "
    "or pane ID. This Windows catalog deliberately supports only session-scoped "
    "read-only discovery; creation, pane capture, input, waits, and global pane "
    "queries are not advertised.";
#else
constexpr std::string_view kInstructions =
    "Start with inspect_tmux. Prefer stable IDs. Use send_text for literal text, "
    "send_keys for named keys, and wait_for_text for future output. Tool "
    "annotations distinguish reads from actions.";
#endif

template <class... Functions> struct Overloaded : Functions... {
  using Functions::operator()...;
};

[[nodiscard]] json encode(const StructuredValue& value) {
  return std::visit(Overloaded{[](std::nullptr_t) { return json(nullptr); },
                               [](bool item) { return json(item); },
                               [](std::int64_t item) { return json(item); },
                               [](const std::string& item) { return json(item); },
                               [](const StructuredValue::Array& items) {
                                 json result = json::array();
                                 for (const auto& item : items) {
                                   result.push_back(encode(item));
                                 }
                                 return result;
                               },
                               [](const StructuredValue::Object& items) {
                                 json result = json::object();
                                 for (const auto& [key, item] : items) {
                                   result[key] = encode(item);
                                 }
                                 return result;
                               }},
                    value.value);
}

[[nodiscard]] json closed_object(json properties, json required) {
  return json{{"type", "object"},
              {"properties", std::move(properties)},
              {"required", std::move(required)},
              {"additionalProperties", false}};
}

[[nodiscard]] json session_schema() {
  return closed_object({{"attached", {{"type", "boolean"}}},
                        {"id", {{"type", "string"}, {"pattern", R"(^\$[0-9]+$)"}}},
                        {"name", {{"type", "string"}}},
                        {"window_count", {{"type", "integer"}, {"minimum", 0}}}},
                       {"attached", "id", "name", "window_count"});
}

[[nodiscard]] json window_schema() {
  return closed_object(
      {{"active", {{"type", "boolean"}}},
       {"id", {{"type", "string"}, {"pattern", R"(^@[0-9]+$)"}}},
       {"index", {{"type", "integer"}}},
       {"name", {{"type", "string"}}},
       {"session_id", {{"type", "string"}, {"pattern", R"(^\$[0-9]+$)"}}}},
      {"active", "id", "index", "name", "session_id"});
}

[[nodiscard]] json pane_schema() {
  return closed_object(
      {{"active", {{"type", "boolean"}}},
       {"command", {{"type", "string"}}},
       {"id", {{"type", "string"}, {"pattern", R"(^%[0-9]+$)"}}},
       {"session_id", {{"type", "string"}, {"pattern", R"(^\$[0-9]+$)"}}},
       {"window_id", {{"type", "string"}, {"pattern", R"(^@[0-9]+$)"}}}},
      {"active", "command", "id", "session_id", "window_id"});
}

[[nodiscard]] json array_property(json items) {
  return json{{"type", "array"}, {"items", std::move(items)}};
}

[[nodiscard]] json output_schema(OutputShape shape) {
  switch (shape) {
  case OutputShape::overview:
    return closed_object({{"panes", array_property(pane_schema())},
                          {"sessions", array_property(session_schema())},
                          {"windows", array_property(window_schema())}},
                         {"panes", "sessions", "windows"});
  case OutputShape::sessions:
    return closed_object({{"sessions", array_property(session_schema())}},
                         {"sessions"});
  case OutputShape::windows:
    return closed_object({{"windows", array_property(window_schema())}}, {"windows"});
  case OutputShape::panes:
    return closed_object({{"panes", array_property(pane_schema())}}, {"panes"});
  case OutputShape::pane_text:
    return closed_object(
        {{"pane_id", {{"type", "string"}, {"pattern", R"(^%[0-9]+$)"}}},
         {"text", {{"type", "string"}}}},
        {"pane_id", "text"});
  case OutputShape::pane_id:
    return closed_object(
        {{"pane_id", {{"type", "string"}, {"pattern", R"(^%[0-9]+$)"}}}}, {"pane_id"});
  case OutputShape::session_id:
    return closed_object(
        {{"name", {{"type", "string"}}},
         {"session_id", {{"type", "string"}, {"pattern", R"(^\$[0-9]+$)"}}}},
        {"name", "session_id"});
  case OutputShape::window_id:
    return closed_object(
        {{"session_id", {{"type", "string"}, {"pattern", R"(^\$[0-9]+$)"}}},
         {"window_id", {{"type", "string"}, {"pattern", R"(^@[0-9]+$)"}}}},
        {"session_id", "window_id"});
  case OutputShape::wait:
    return closed_object(
        {{"elapsed_ms", {{"type", "integer"}, {"minimum", 0}}},
         {"matched", {{"type", "boolean"}}},
         {"mode", {{"type", "string"}}},
         {"pane_id", {{"type", "string"}, {"pattern", R"(^%[0-9]+$)"}}},
         {"text", {{"type", "string"}}},
         {"timed_out", {{"type", "boolean"}}}},
        {"elapsed_ms", "matched", "mode", "pane_id", "text", "timed_out"});
  case OutputShape::matches:
    return closed_object(
        {{"matches",
          array_property(closed_object(
              {{"line", {{"type", "string"}}},
               {"pane_id", {{"type", "string"}, {"pattern", R"(^%[0-9]+$)"}}}},
              {"line", "pane_id"}))}},
        {"matches"});
  }
  return closed_object({}, {});
}

[[nodiscard]] json describe(const Tool& tool) {
  json properties = json::object();
  for (const Parameter& parameter : tool.parameters) {
    json property{
        {"type", parameter.type == ArgumentType::integer ? "integer" : "string"},
        {"description", parameter.description}};
    if (parameter.minimum.has_value()) {
      property["minimum"] = *parameter.minimum;
    }
    if (parameter.maximum.has_value()) {
      property["maximum"] = *parameter.maximum;
    }
    if (parameter.maximum_length.has_value()) {
      property["maxLength"] = *parameter.maximum_length;
    }
    properties[parameter.name] = std::move(property);
  }
  return json{
      {"name", tool.name},
      {"title", tool.title},
      {"description", tool.description},
      {"inputSchema", closed_object(std::move(properties), tool.required_names())},
      {"outputSchema", output_schema(tool.output)},
      {"annotations",
       {{"title", tool.title},
        {"readOnlyHint", tool.annotations.read_only},
        {"destructiveHint", tool.annotations.destructive},
        {"idempotentHint", tool.annotations.idempotent},
        {"openWorldHint", tool.annotations.open_world}}}};
}

[[nodiscard]] json implementation() {
  return {{"name", "libtmux-cxx"},
          {"title", "libtmux C++ tmux server"},
          {"version", std::string{libtmux::library_version()}}};
}

void stamp_modern(json& result) {
  result["resultType"] = "complete";
  result["_meta"] = {{"io.modelcontextprotocol/serverInfo", implementation()}};
}

[[nodiscard]] json listed_tools(const ToolSet& tools) {
  json listed = json::array();
  for (const Tool& tool : tools.tools()) {
    listed.push_back(describe(tool));
  }
  return listed;
}

} // namespace

json modern_protocol_versions() { return json::array({kModernProtocolVersion}); }

json initialize_result(std::string_view version) {
  return {{"protocolVersion", version},
          {"capabilities", {{"tools", json::object()}}},
          {"serverInfo", implementation()},
          {"instructions", kInstructions}};
}

json discover_result() {
  json result{{"supportedVersions", modern_protocol_versions()},
              {"capabilities", {{"tools", json::object()}}},
              {"instructions", kInstructions},
              {"ttlMs", 3600000},
              {"cacheScope", "public"}};
  stamp_modern(result);
  return result;
}

json ping_result() { return json::object(); }

json tools_result(const ToolSet& tools, ProtocolEra era) {
  json result{{"tools", listed_tools(tools)}};
  if (era == ProtocolEra::modern) {
    result["ttlMs"] = 3600000;
    result["cacheScope"] = "public";
    stamp_modern(result);
  }
  return result;
}

json tool_success(const ToolOutput& answer, ProtocolEra era) {
  const json structured = encode(StructuredValue{answer.structured});
  json result{
      {"content", json::array({json{{"type", "text"}, {"text", structured.dump()}}})},
      {"structuredContent", structured},
      {"isError", false}};
  if (era == ProtocolEra::modern) {
    stamp_modern(result);
  }
  return result;
}

json tool_failure(std::string message, ProtocolEra era) {
  json result{
      {"content", json::array({json{{"type", "text"}, {"text", std::move(message)}}})},
      {"isError", true}};
  if (era == ProtocolEra::modern) {
    stamp_modern(result);
  }
  return result;
}

} // namespace libtmux::mcp::server
