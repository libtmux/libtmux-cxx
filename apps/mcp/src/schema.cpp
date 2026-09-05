#include "schema.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "libtmux/version.hpp"
#include "tool_support.hpp"

namespace libtmux::mcp::server {
namespace {

constexpr std::string_view kInstructions =
    "Start with list_sessions and retain stable IDs. This process uses one pinned "
    "tmux socket; read tmux://capabilities for its resolved path, attach command, "
    "configuration provenance, and effective tools. Execute tools run pane "
    "processes with this user's permissions; tool filtering is not an OS sandbox.";
constexpr std::string_view kCapabilityMetadata = "com.git-pull.libtmux-mcp/capability";

[[nodiscard]] std::string shell_quote(std::string_view value) {
  std::string quoted{"'"};
  for (const char character : value) {
    if (character == '\'') {
      quoted += "'\"'\"'";
    } else {
      quoted += character;
    }
  }
  quoted += '\'';
  return quoted;
}

[[nodiscard]] json optional_string(const std::optional<std::string>& value) {
  return value.has_value() ? json(*value) : json(nullptr);
}

[[nodiscard]] json capability_row(const ToolDefinition& tool,
                                  const ToolRegistry& tools);

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
  return closed_object(
      {{"attached", {{"type", "boolean"}}},
       {"client_count", {{"type", "integer"}, {"minimum", 0}}},
       {"id", {{"type", "string"}, {"pattern", R"(^\$[0-9]+$)"}}},
       {"name", {{"type", "string"}}},
       {"path", {{"type", "string"}}},
       {"window_count", {{"type", "integer"}, {"minimum", 0}}}},
      {"attached", "client_count", "id", "name", "path", "window_count"});
}

[[nodiscard]] json window_schema() {
  return closed_object(
      {{"active", {{"type", "boolean"}}},
       {"height", {{"type", "integer"}, {"minimum", 0}}},
       {"id", {{"type", "string"}, {"pattern", R"(^@[0-9]+$)"}}},
       {"index", {{"type", "integer"}}},
       {"layout", {{"type", "string"}}},
       {"name", {{"type", "string"}}},
       {"pane_count", {{"type", "integer"}, {"minimum", 0}}},
       {"session_id", {{"type", "string"}, {"pattern", R"(^\$[0-9]+$)"}}},
       {"width", {{"type", "integer"}, {"minimum", 0}}}},
      {"active", "height", "id", "index", "layout", "name", "pane_count", "session_id",
       "width"});
}

[[nodiscard]] json pane_schema() {
  return closed_object(
      {{"active", {{"type", "boolean"}}},
       {"command", {{"type", "string"}}},
       {"dead", {{"type", "boolean"}}},
       {"height", {{"type", "integer"}, {"minimum", 0}}},
       {"id", {{"type", "string"}, {"pattern", R"(^%[0-9]+$)"}}},
       {"index", {{"type", "integer"}, {"minimum", 0}}},
       {"path", {{"type", "string"}}},
       {"pid", {{"type", "integer"}, {"minimum", 0}}},
       {"session_id", {{"type", "string"}, {"pattern", R"(^\$[0-9]+$)"}}},
       {"title", {{"type", "string"}}},
       {"width", {{"type", "integer"}, {"minimum", 0}}},
       {"window_id", {{"type", "string"}, {"pattern", R"(^@[0-9]+$)"}}}},
      {"active", "command", "dead", "height", "id", "index", "path", "pid",
       "session_id", "title", "width", "window_id"});
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
        {{"changed", {{"type", "boolean"}}},
         {"pane_id", {{"type", "string"}, {"pattern", R"(^%[0-9]+$)"}}}},
        {"pane_id"});
  case OutputShape::pane_targets:
    return closed_object(
        {{"pane_id", {{"type", "string"}, {"pattern", R"(^%[0-9]+$)"}}},
         {"target_pane_ids",
          array_property({{"type", "string"}, {"pattern", R"(^%[0-9]+$)"}})}},
        {"pane_id", "target_pane_ids"});
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
        {"elapsed_ms", "matched", "mode", "text", "timed_out"});
  case OutputShape::matches:
    return closed_object(
        {{"matches",
          array_property(closed_object(
              {{"line", {{"type", "string"}}},
               {"pane_id", {{"type", "string"}, {"pattern", R"(^%[0-9]+$)"}}}},
              {"line", "pane_id"}))}},
        {"matches"});
  case OutputShape::object:
    return json{{"type", "object"}, {"additionalProperties", true}};
  }
  return closed_object({}, {});
}

[[nodiscard]] json input_schema(const ToolDefinition& tool, const ToolRegistry& tools) {
  json properties = json::object();
  for (const Parameter& parameter : tool.schema.input) {
    if (parameter.type == ArgumentType::string_array) {
      json items{{"type", "string"}};
      if (parameter.maximum_length.has_value()) {
        items["maxLength"] = *parameter.maximum_length;
      }
      json property{{"type", "array"},
                    {"description", parameter.description},
                    {"items", std::move(items)}};
      if (parameter.minimum.has_value()) {
        property["minItems"] = *parameter.minimum;
      }
      if (parameter.maximum.has_value()) {
        property["maxItems"] = *parameter.maximum;
      }
      properties[parameter.name] = std::move(property);
      continue;
    }
    if (parameter.type == ArgumentType::send_key_operations) {
      const json operation = closed_object(
          {{"paneId", {{"type", "string"}, {"maxLength", detail::kTargetCharacters}}},
           {"keys", {{"type", "string"}, {"maxLength", detail::kSearchCharacters}}},
           {"enter", {{"type", "boolean"}}},
           {"literal", {{"type", "boolean"}}}},
          {"paneId", "keys"});
      properties[parameter.name] = {{"type", "array"},
                                    {"description", parameter.description},
                                    {"minItems", 1},
                                    {"maxItems", 64},
                                    {"items", operation}};
      continue;
    }
    if (parameter.type == ArgumentType::read_calls) {
      json alternatives = json::array();
      for (const std::string& name : tool.authority.nested_tools) {
        const ToolDefinition* const nested = tools.find_nested(tool, name);
        if (nested == nullptr) {
          continue;
        }
        alternatives.push_back(
            closed_object({{"tool", {{"type", "string"}, {"const", name}}},
                           {"arguments", input_schema(*nested, tools)}},
                          {"tool"}));
      }
      json calls = {{"type", "array"},
                    {"description", parameter.description},
                    {"minItems", 1},
                    {"maxItems", 16}};
      calls["items"] =
          alternatives.empty() ? json(false) : json{{"oneOf", std::move(alternatives)}};
      properties[parameter.name] = std::move(calls);
      continue;
    }
    std::string_view type = "string";
    if (parameter.type == ArgumentType::integer) {
      type = "integer";
    } else if (parameter.type == ArgumentType::boolean) {
      type = "boolean";
    }
    json property{{"type", type}, {"description", parameter.description}};
    if (parameter.minimum.has_value()) {
      property["minimum"] = *parameter.minimum;
    }
    if (parameter.maximum.has_value()) {
      property["maximum"] = *parameter.maximum;
    }
    if (parameter.maximum_length.has_value()) {
      property["maxLength"] = *parameter.maximum_length;
    }
    if (!parameter.allowed_values.empty()) {
      property["enum"] = parameter.allowed_values;
    }
    properties[parameter.name] = std::move(property);
  }
  return closed_object(std::move(properties), tool.required_names());
}

[[nodiscard]] json annotations(const ToolDefinition& tool) {
  return {{"title", tool.title},
          {"readOnlyHint", tool.annotations.read_only},
          {"destructiveHint", tool.annotations.destructive},
          {"idempotentHint", tool.annotations.idempotent},
          {"openWorldHint", tool.annotations.open_world}};
}

[[nodiscard]] json describe(const ToolDefinition& tool, const ToolRegistry& tools) {
  return json{{"name", tool.name},
              {"title", tool.title},
              {"description", tool.description},
              {"inputSchema", input_schema(tool, tools)},
              {"outputSchema", output_schema(tool.schema.output)},
              {"annotations", annotations(tool)},
              {"_meta", {{kCapabilityMetadata, capability_row(tool, tools)}}}};
}

[[nodiscard]] std::string_view name(Toolset value) {
  switch (value) {
  case Toolset::inspect:
    return "inspect";
  case Toolset::manage:
    return "manage";
  case Toolset::execute:
    return "execute";
  case Toolset::teardown:
    return "teardown";
  }
  return {};
}

[[nodiscard]] std::string_view name(ProcessReach value) {
  switch (value) {
  case ProcessReach::none:
    return "none";
  case ProcessReach::configured_process:
    return "configured-process";
  case ProcessReach::pane_input:
    return "pane-input";
  case ProcessReach::pane_command:
    return "pane-command";
  }
  return {};
}

[[nodiscard]] std::string_view name(Effect value) {
  switch (value) {
  case Effect::observe:
    return "observe";
  case Effect::change:
    return "change";
  case Effect::delete_:
    return "delete";
  }
  return {};
}

[[nodiscard]] std::string_view name(OutputClass value) {
  switch (value) {
  case OutputClass::tmux_metadata:
    return "tmux-metadata";
  case OutputClass::terminal_content:
    return "terminal-content";
  case OutputClass::process_environment:
    return "process-environment";
  case OutputClass::configured_command:
    return "configured-command";
  }
  return {};
}

[[nodiscard]] std::string_view name(InputControl value) {
  switch (value) {
  case InputControl::none:
    return "none";
  case InputControl::double_hash_once:
    return "double-hash-once";
  case InputControl::validated_variable_name:
    return "validated-variable-name";
  }
  return {};
}

[[nodiscard]] json capability_row(const ToolDefinition& tool,
                                  const ToolRegistry& tools) {
  json effects = json::array();
  for (const Effect effect : tool.authority.effects) {
    effects.push_back(name(effect));
  }
  json outputs = json::array();
  for (const OutputClass output : tool.authority.output_classes) {
    outputs.push_back(name(output));
  }
  json input_literalization = json::object();
  for (const auto& [field, sinks] : tool.authority.input_sinks) {
    for (const Sink& sink : sinks) {
      if (sink.type == InputSink::tmux_format && sink.control != InputControl::none) {
        input_literalization[field] = name(sink.control);
      }
    }
  }
  json nested = json::array();
  for (const std::string& nested_name : tool.authority.nested_tools) {
    nested.push_back(nested_name);
  }
  return {{"name", tool.name},
          {"title", tool.title},
          {"description", tool.description},
          {"toolset", name(tool.toolset)},
          {"processReach", name(tool.authority.process_reach)},
          {"tmuxEffects", std::move(effects)},
          {"outputClasses", std::move(outputs)},
          {"mayExposeSecrets", tool.authority.may_expose_secrets},
          {"mayReturnUntrustedContent", tool.authority.may_return_untrusted_content},
          {"amplifiesFutureInput", tool.authority.amplifies_future_input},
          {"annotations", annotations(tool)},
          {"inputLiteralization", std::move(input_literalization)},
          {"nestedAuthority", std::move(nested)},
          {"inputSchema", input_schema(tool, tools)},
          {"outputSchema", output_schema(tool.schema.output)}};
}

[[nodiscard]] json capability_document(const ToolRegistry& tools,
                                       const CapabilityDisclosure& disclosure) {
  json rows = json::array();
  json effective = json::array();
  for (const ToolDefinition& tool : tools.tools()) {
    effective.push_back(tool.name);
    rows.push_back(capability_row(tool, tools));
  }
  json toolsets = json::array();
  for (const Toolset toolset : tools.selection().toolsets) {
    toolsets.push_back(name(toolset));
  }
  json included = json::array();
  for (const std::string& tool : tools.selection().include) {
    included.push_back(tool);
  }
  json excluded = json::array();
  for (const std::string& tool : tools.selection().exclude) {
    excluded.push_back(tool);
  }
  json connection{
      {"socketSelector", disclosure.selector},
      {"socketProvenance", disclosure.selection_provenance},
      {"resolvedSocketPath", optional_string(disclosure.resolved_socket_path)},
      {"serverState", disclosure.server_state},
      {"configurationProvenance", disclosure.configuration_provenance},
      {"attachCommand", optional_string(disclosure.attach_command)}};
  if (disclosure.namespace_selector.has_value()) {
    connection["namespaceSelector"] = *disclosure.namespace_selector;
  }
  return {{"schemaVersion", 1},
          {"frozen", true},
          {"socket",
           {{"selector", disclosure.selector},
            {"selectionProvenance", disclosure.selection_provenance},
            {"serverState", disclosure.server_state},
            {"configurationProvenance", disclosure.configuration_provenance},
            {"namespaceBoundary", "tmux-objects-only"}}},
          {"boundary",
           {{"oneSocketPerProcess", true},
            {"perCallSocketSelection", false},
            {"hostCommandExecution", false},
            {"dynamicResources", false}}},
          {"connection", std::move(connection)},
          {"toolsets", std::move(toolsets)},
          {"includedTools", std::move(included)},
          {"excludedTools", std::move(excluded)},
          {"toolCount", tools.tools().size()},
          {"effectiveTools", std::move(effective)},
          {"hostCommandTools", 0},
          {"toolFilteringBoundary", "interface-shaping-not-authorization"},
          {"executionAuthority", "tmux-user"},
          {"operatingSystemBoundary", "none"},
          {"tools", std::move(rows)}};
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

[[nodiscard]] json complete_tool_result(const json& structured, ProtocolEra era) {
  json result{
      {"content", json::array({json{{"type", "text"}, {"text", structured.dump()}}})},
      {"structuredContent", structured},
      {"isError", false}};
  if (era == ProtocolEra::modern) {
    stamp_modern(result);
  }
  return result;
}

[[nodiscard]] json bounded_tool_result(json structured, ProtocolEra era,
                                       std::size_t maximum_bytes) {
  json result = complete_tool_result(structured, era);
  std::size_t estimated_size = result.dump().size();
  if (estimated_size <= maximum_bytes) {
    return result;
  }
  const std::size_t original_size = estimated_size;
  auto rows = structured.find("results");
  if (rows == structured.end() || !rows->is_array()) {
    return result;
  }
  constexpr std::size_t metadata_headroom = 64U;
  const std::size_t target =
      maximum_bytes > metadata_headroom ? maximum_bytes - metadata_headroom : 0U;
  const auto discount = [&estimated_size](std::size_t serialized_savings) {
    const std::size_t envelope_savings = serialized_savings * 2U;
    estimated_size =
        envelope_savings < estimated_size ? estimated_size - envelope_savings : 0U;
  };
  for (json& row : *rows) {
    if (estimated_size <= target) {
      break;
    }
    auto nested = row.find("result");
    if (nested == row.end() || nested->is_null()) {
      continue;
    }
    const std::size_t before = nested->dump().size();
    *nested = nullptr;
    row["resultTruncated"] = true;
    discount(before > 4U ? before - 4U : 0U);
  }
  constexpr std::string_view truncated_error =
      "nested error text truncated to fit the response limit";
  for (json& row : *rows) {
    if (estimated_size <= target) {
      break;
    }
    auto error = row.find("error");
    if (error == row.end() || !error->is_string() ||
        error->get_ref<const std::string&>() == truncated_error) {
      continue;
    }
    const std::size_t before = error->dump().size();
    *error = truncated_error;
    row["resultTruncated"] = true;
    const std::size_t after = error->dump().size();
    discount(before > after ? before - after : 0U);
  }
  structured["truncated"] = true;
  structured["truncatedBytes"] = 0;
  for (int iteration = 0; iteration < 4; ++iteration) {
    result = complete_tool_result(structured, era);
    const std::size_t current_size = result.dump().size();
    const std::size_t dropped =
        original_size > current_size ? original_size - current_size : 0U;
    if (structured["truncatedBytes"] == dropped) {
      break;
    }
    structured["truncatedBytes"] = dropped;
  }
  result = complete_tool_result(structured, era);
  return result;
}

[[nodiscard]] json listed_tools(const ToolRegistry& tools) {
  json listed = json::array();
  for (const ToolDefinition& tool : tools.tools()) {
    listed.push_back(describe(tool, tools));
  }
  return listed;
}

} // namespace

CapabilityDisclosure capability_disclosure(libtmux::ServerImplementation implementation,
                                           std::string selector,
                                           std::string selection_provenance,
                                           std::string server_state,
                                           std::string configuration_provenance,
                                           std::string resolved_endpoint) {
  CapabilityDisclosure disclosure{
      .selector = std::move(selector),
      .selection_provenance = std::move(selection_provenance),
      .server_state = std::move(server_state),
      .configuration_provenance = std::move(configuration_provenance),
      .namespace_selector = std::nullopt,
      .resolved_socket_path = std::nullopt,
      .attach_command = std::nullopt,
  };
  if (implementation == libtmux::ServerImplementation::tmux) {
    disclosure.attach_command =
        "tmux -N -S " + shell_quote(resolved_endpoint) + " attach";
    disclosure.resolved_socket_path = std::move(resolved_endpoint);
  } else if (implementation == libtmux::ServerImplementation::psmux) {
    disclosure.namespace_selector = std::move(resolved_endpoint);
  }
  return disclosure;
}

json modern_protocol_versions() { return json::array({kModernProtocolVersion}); }

json initialize_result(std::string_view version) {
  return {{"protocolVersion", version},
          {"capabilities",
           {{"tools", json::object()},
            {"resources", {{"listChanged", false}, {"subscribe", false}}}}},
          {"serverInfo", implementation()},
          {"instructions", kInstructions}};
}

json discover_result() {
  json result{{"supportedVersions", modern_protocol_versions()},
              {"capabilities",
               {{"tools", json::object()},
                {"resources", {{"listChanged", false}, {"subscribe", false}}}}},
              {"instructions", kInstructions},
              {"ttlMs", 3600000},
              {"cacheScope", "public"}};
  stamp_modern(result);
  return result;
}

json ping_result() { return json::object(); }

json tools_result(const ToolRegistry& tools, ProtocolEra era) {
  json result{{"tools", listed_tools(tools)}};
  if (era == ProtocolEra::modern) {
    result["ttlMs"] = 3600000;
    result["cacheScope"] = "public";
    stamp_modern(result);
  }
  return result;
}

json resources_result(ProtocolEra era) {
  json result{{"resources",
               json::array({{{"uri", "tmux://capabilities"},
                             {"name", "tmux-capabilities"},
                             {"title", "Effective tmux capabilities"},
                             {"description",
                              "The startup-frozen effective tool capability manifest."},
                             {"mimeType", "application/json"}}})}};
  if (era == ProtocolEra::modern) {
    result["ttlMs"] = 3600000;
    result["cacheScope"] = "public";
    stamp_modern(result);
  }
  return result;
}

json capabilities_resource_result(const ToolRegistry& tools, ProtocolEra era,
                                  const CapabilityDisclosure& disclosure) {
  json result{
      {"contents",
       json::array({{{"uri", "tmux://capabilities"},
                     {"mimeType", "application/json"},
                     {"text", capability_document(tools, disclosure).dump()}}})}};
  if (era == ProtocolEra::modern) {
    result["ttlMs"] = 3600000;
    result["cacheScope"] = "public";
    stamp_modern(result);
  }
  return result;
}

json tool_success(const ToolOutput& answer, ProtocolEra era) {
  json structured = encode(StructuredValue{answer.structured});
  if (answer.maximum_response_bytes.has_value()) {
    return bounded_tool_result(std::move(structured), era,
                               *answer.maximum_response_bytes);
  }
  return complete_tool_result(structured, era);
}

json tool_success(const ToolOutput& answer, ProtocolEra era,
                  std::size_t maximum_result_bytes) {
  return bounded_tool_result(encode(StructuredValue{answer.structured}), era,
                             maximum_result_bytes);
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
