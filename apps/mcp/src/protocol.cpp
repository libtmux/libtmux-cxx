#include "protocol.hpp"

#include <initializer_list>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include "protocol_validation.hpp"
#include "schema.hpp"

namespace libtmux::mcp::server {
json failure(const json& id, int code, std::string message, std::optional<json> data) {
  json error{{"code", code}, {"message", std::move(message)}};
  if (data.has_value()) {
    error["data"] = *std::move(data);
  }
  return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", std::move(error)}};
}

json success(const json& id, json result) {
  return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

std::string request_key(const json& id) {
  return id.is_string() ? "s:" + id.get<std::string>() : "n:" + id.dump();
}

bool request_id(const json& value) {
  return value.is_string() || value.is_number_integer() || value.is_number_unsigned();
}

bool is_legacy_protocol_version(std::string_view version) {
  return std::ranges::find(kLegacyProtocolVersions, version) !=
         kLegacyProtocolVersions.end();
}

Route Route::answering(json message) {
  Route route;
  route.reply = std::move(message);
  return route;
}

Route Route::dispatching(CallRequest request) {
  Route route;
  route.call = std::move(request);
  return route;
}

Route Route::cancelling(json id) {
  Route route;
  route.cancellation = std::move(id);
  return route;
}

ProtocolSession::ProtocolSession(libtmux::Server server)
    : server_{std::move(server)}, tools_{default_tools()} {}

Route ProtocolSession::route(const json& request) {
  if (!request.is_object()) {
    return Route::answering(
        failure(nullptr, kInvalidRequest, "a request must be a JSON object"));
  }
  const bool notification = !request.contains("id");
  const json* identifier = nullptr;
  if (!notification) {
    const auto found = request.find("id");
    if (found == request.end() || !request_id(*found)) {
      return Route::answering(
          failure(nullptr, kInvalidRequest, "an id must be a string or integer"));
    }
    identifier = &*found;
  }
  const json id = identifier == nullptr ? json(nullptr) : *identifier;
  const auto version = request.find("jsonrpc");
  if (version == request.end() || !version->is_string() || *version != "2.0") {
    return Route::answering(failure(id, kInvalidRequest, "not a JSON-RPC 2.0 request"));
  }
  const auto named = request.find("method");
  if (named == request.end() || !named->is_string()) {
    return Route::answering(
        failure(id, kInvalidRequest, "a request needs a method name"));
  }
  const auto supplied = request.find("params");
  if (supplied != request.end() && !supplied->is_object()) {
    return notification ? Route{}
                        : Route::answering(
                              failure(id, kInvalidParams, "params must be an object"));
  }
  const json params = supplied == request.end() ? json::object() : *supplied;
  const std::string method = named->get<std::string>();
  const bool modern_candidate = era_ == ProtocolEra::modern ||
                                method == "server/discover" ||
                                declares_modern_metadata(params);

  if (method == "initialize") {
    if (era_ == ProtocolEra::modern) {
      return notification ? Route{}
                          : Route::answering(failure(
                                id, kInvalidRequest,
                                "cannot mix initialize with modern MCP requests"));
    }
    if (declares_modern_metadata(params)) {
      if (era_ == ProtocolEra::legacy) {
        return notification ? Route{}
                            : Route::answering(failure(
                                  id, kInvalidRequest,
                                  "cannot mix initialize with modern MCP requests"));
      }
      if (notification) {
        return {};
      }
      auto metadata = validate_modern_metadata(params);
      if (!metadata.has_value()) {
        return Route::answering(failure(id, metadata.error().code,
                                        metadata.error().message,
                                        metadata.error().data));
      }
      era_ = ProtocolEra::modern;
      return Route::answering(
          failure(id, kMethodNotFound, "no such method: initialize"));
    }
    return initialize(id, params, notification);
  }
  if (method == "notifications/initialized") {
    return initialized(id, params, notification);
  }

  ProtocolEra request_era = ProtocolEra::legacy;
  std::optional<json> modern_progress;
  if (!notification && modern_candidate) {
    if (era_ == ProtocolEra::legacy) {
      return Route::answering(
          failure(id, kInvalidRequest,
                  "cannot mix modern requests with an initialized legacy process"));
    }
    auto metadata = validate_modern_metadata(params);
    if (!metadata.has_value()) {
      return Route::answering(failure(id, metadata.error().code,
                                      metadata.error().message, metadata.error().data));
    }
    if (era_ == ProtocolEra::undecided) {
      era_ = ProtocolEra::modern;
    }
    modern_progress = *std::move(metadata);
    request_era = ProtocolEra::modern;
  } else if (!notification && era_ == ProtocolEra::modern) {
    return Route::answering(failure(
        id, kInvalidParams, "modern MCP requests require per-request metadata"));
  }

  if (method == "ping") {
    if (notification) {
      return {};
    }
    if (request_era == ProtocolEra::modern) {
      return Route::answering(failure(id, kMethodNotFound, "no such method: ping"));
    }
    if (const auto key = unexpected_key(params, {"_meta"}); key.has_value()) {
      return Route::answering(
          failure(id, kInvalidParams, "unknown ping parameter: " + *key));
    }
    return valid_legacy_metadata(params)
               ? Route::answering(success(id, ping_result()))
               : Route::answering(failure(id, kInvalidParams, "ping _meta is invalid"));
  }
  if (method == "server/discover") {
    return discover(id, params, notification);
  }

  const bool ready =
      era_ == ProtocolEra::modern ||
      (era_ == ProtocolEra::legacy && legacy_state_ == LegacyState::ready);
  if (!ready) {
    return notification ? Route{}
                        : Route::answering(failure(id, kNotInitialized,
                                                   "server is not initialized"));
  }
  if (method == "notifications/cancelled") {
    if (!notification) {
      return Route::answering(failure(
          id, kInvalidRequest, "notifications/cancelled must be a notification"));
    }
    return cancelled(params, notification);
  }
  if (method == "tools/list") {
    return list_tools(id, params, notification, request_era);
  }
  if (method == "tools/call") {
    Route action = call_tool(id, params, notification, request_era);
    if (action.call.has_value() && request_era == ProtocolEra::modern) {
      action.call->progress_token = std::move(modern_progress);
    }
    return action;
  }
  return notification ? Route{}
                      : Route::answering(
                            failure(id, kMethodNotFound, "no such method: " + method));
}

json ProtocolSession::execute(const CallRequest& request, const CallContext& context) {
  const auto name = request.params.find("name");
  if (name == request.params.end() || !name->is_string() || name->empty()) {
    return failure(request.id, kInvalidParams, "tools/call needs a tool name");
  }
  const Tool* const tool = tools_.find(name->get<std::string>());
  if (tool == nullptr) {
    return failure(request.id, kInvalidParams,
                   "unknown tool: " + name->get<std::string>());
  }
  auto arguments = read_arguments(request.params, *tool);
  if (!arguments.has_value()) {
    if (request.tool_input_errors_are_results && arguments.error().tool_input) {
      return success(request.id, tool_failure(arguments.error().message, request.era));
    }
    return failure(request.id, kInvalidParams, arguments.error().message);
  }
  const auto answer = tools_.call(server_, tool->name, *arguments, context);
  if (answer.has_value()) {
    return success(request.id, tool_success(*answer, request.era));
  }
  if (answer.error().caller_error) {
    if (request.tool_input_errors_are_results) {
      return success(request.id, tool_failure(answer.error().message, request.era));
    }
    return failure(request.id, kInvalidParams, answer.error().message);
  }
  return success(request.id, tool_failure(answer.error().message, request.era));
}

bool ProtocolSession::accepts_batches() const noexcept {
  return era_ == ProtocolEra::legacy && legacy_state_ == LegacyState::ready &&
         legacy_version_ == kBatchLegacyProtocolVersion;
}

bool ProtocolSession::uses_legacy_request_ids() const noexcept {
  return era_ == ProtocolEra::legacy;
}

Route ProtocolSession::initialize(const json& id, const json& params,
                                  bool notification) {
  if (notification) {
    return {};
  }
  if (era_ == ProtocolEra::legacy && legacy_state_ != LegacyState::fresh) {
    return Route::answering(
        failure(id, kInvalidRequest, "initialize may be sent only once"));
  }
  if (const auto key = unexpected_key(
          params, {"protocolVersion", "capabilities", "clientInfo", "_meta"});
      key.has_value()) {
    return Route::answering(
        failure(id, kInvalidParams, "unknown initialize parameter: " + *key));
  }
  if (!valid_legacy_metadata(params)) {
    return Route::answering(failure(id, kInvalidParams, "initialize _meta is invalid"));
  }
  const auto protocol = params.find("protocolVersion");
  const auto capabilities = params.find("capabilities");
  const auto client = params.find("clientInfo");
  if (protocol == params.end() || !protocol->is_string() || protocol->empty()) {
    return Route::answering(
        failure(id, kInvalidParams, "initialize needs protocolVersion"));
  }
  // Extracted rather than compared as JSON: MSVC finds json's operator==
  // overloads ambiguous against a string_view.
  if (protocol->get<std::string>() == kModernProtocolVersion) {
    return Route::answering(
        failure(id, kInvalidRequest,
                "2026-07-28 uses server/discover and per-request metadata"));
  }
  if (capabilities == params.end() || !capabilities->is_object()) {
    return Route::answering(
        failure(id, kInvalidParams, "initialize needs client capabilities"));
  }
  if (client == params.end() || !valid_implementation(*client)) {
    return Route::answering(
        failure(id, kInvalidParams, "initialize needs valid clientInfo"));
  }
  const std::string requested = protocol->get<std::string>();
  legacy_version_ = is_legacy_protocol_version(requested)
                        ? requested
                        : std::string{kLatestLegacyProtocolVersion};
  era_ = ProtocolEra::legacy;
  legacy_state_ = LegacyState::awaiting_initialized;
  return Route::answering(success(id, initialize_result(legacy_version_)));
}

Route ProtocolSession::initialized(const json& id, const json& params,
                                   bool notification) {
  if (!notification) {
    return Route::answering(failure(
        id, kInvalidRequest, "notifications/initialized must be a notification"));
  }
  if (era_ == ProtocolEra::modern || !only_keys(params, {"_meta"}) ||
      !metadata_object(params)) {
    return {};
  }
  if (era_ == ProtocolEra::legacy &&
      legacy_state_ == LegacyState::awaiting_initialized) {
    legacy_state_ = LegacyState::ready;
  }
  return {};
}

Route ProtocolSession::discover(const json& id, const json& params, bool notification) {
  if (notification) {
    return {};
  }
  if (const auto key = unexpected_key(params, {"_meta"}); key.has_value()) {
    return Route::answering(
        failure(id, kInvalidParams, "unknown server/discover parameter: " + *key));
  }
  return Route::answering(success(id, discover_result()));
}

Route ProtocolSession::cancelled(const json& params, bool notification) const {
  if (!notification || !only_keys(params, {"requestId", "reason", "_meta"}) ||
      !metadata_object(params)) {
    return {};
  }
  const auto id = params.find("requestId");
  const auto reason = params.find("reason");
  if (id == params.end() || !request_id(*id) ||
      (reason != params.end() && !reason->is_string())) {
    return {};
  }
  return Route::cancelling(*id);
}

Route ProtocolSession::list_tools(const json& id, const json& params, bool notification,
                                  ProtocolEra era) const {
  if (notification) {
    return {};
  }
  if (const auto key = unexpected_key(params, {"cursor", "_meta"}); key.has_value()) {
    return Route::answering(
        failure(id, kInvalidParams, "unknown tools/list parameter: " + *key));
  }
  if (era == ProtocolEra::legacy && !valid_legacy_metadata(params)) {
    return Route::answering(failure(id, kInvalidParams, "tools/list _meta is invalid"));
  }
  if (const auto cursor = params.find("cursor"); cursor != params.end()) {
    if (!cursor->is_string()) {
      return Route::answering(
          failure(id, kInvalidParams, "tools/list cursor must be a string"));
    }
    return Route::answering(
        failure(id, kInvalidParams, "this fixed tool catalog does not paginate"));
  }
  return Route::answering(success(id, tools_result(tools_, era)));
}

Route ProtocolSession::call_tool(const json& id, const json& params, bool notification,
                                 ProtocolEra era) const {
  if (notification) {
    return {};
  }
  const auto key = era == ProtocolEra::modern
                       ? unexpected_key(params, {"name", "arguments", "inputResponses",
                                                 "requestState", "_meta"})
                       : unexpected_key(params, {"name", "arguments", "_meta"});
  if (key.has_value()) {
    return Route::answering(
        failure(id, kInvalidParams, "unknown tools/call parameter: " + *key));
  }
  if (params.contains("inputResponses") || params.contains("requestState")) {
    return Route::answering(
        failure(id, kInvalidParams, "this server has no pending input round"));
  }

  std::optional<json> progress;
  if (era == ProtocolEra::legacy) {
    if (!valid_legacy_metadata(params)) {
      return Route::answering(
          failure(id, kInvalidParams, "tools/call _meta is invalid"));
    }
    progress = legacy_progress_token(params);
  }
  return Route::dispatching(
      CallRequest{.id = id,
                  .params = params,
                  .progress_token = std::move(progress),
                  .era = era,
                  .tool_input_errors_are_results =
                      era == ProtocolEra::modern ||
                      legacy_version_ == kLatestLegacyProtocolVersion});
}

} // namespace libtmux::mcp::server
