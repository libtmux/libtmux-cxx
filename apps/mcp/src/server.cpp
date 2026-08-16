// The MCP tool surface, as a program an agent can actually run.
//
// Model Context Protocol over stdio is JSON-RPC 2.0, one object per line. This
// reads that, dispatches into the same `ToolSet` the tests drive, and writes
// the reply back — so the protocol lives here and the library above it still
// encodes nothing.
//
// It speaks the three methods a client needs to use tools, and answers
// anything else with the error the specification names, rather than closing
// the connection on a client that asked for something reasonable.

#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "libtmux/server.hpp"
#include "libtmux_consumers/mcp.hpp"

namespace {

using json = nlohmann::json;

// JSON-RPC's own codes, plus the one the specification adds for a tool that
// exists but could not run.
constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;

// The revision of the protocol this speaks. A client that asks for another one
// is told what it got rather than being refused: the specification requires
// the server to answer with the version it will actually use.
constexpr std::string_view kProtocolVersion = "2024-11-05";

json failure(const json& id, int code, std::string message) {
  return json{{"jsonrpc", "2.0"},
              {"id", id},
              {"error", {{"code", code}, {"message", std::move(message)}}}};
}

json success(const json& id, json result) {
  return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

// A tool's arguments arrive as an object of strings. Anything else is the
// caller's mistake, and saying which key is wrong saves them a guess.
libtmux::expected<libtmux::mcp::Arguments, std::string>
read_arguments(const json& params) {
  libtmux::mcp::Arguments arguments;
  const auto found = params.find("arguments");
  if (found == params.end() || found->is_null()) {
    return arguments;
  }
  if (!found->is_object()) {
    return libtmux::unexpected(std::string{"arguments must be an object"});
  }
  for (const auto& [key, value] : found->items()) {
    if (value.is_string()) {
      arguments.emplace(key, value.get<std::string>());
    } else if (value.is_number_integer()) {
      arguments.emplace(key, std::to_string(value.get<long long>()));
    } else if (value.is_boolean()) {
      arguments.emplace(key, value.get<bool>() ? "true" : "false");
    } else {
      return libtmux::unexpected("argument " + key + " must be a string");
    }
  }
  return arguments;
}

json describe(const libtmux::mcp::Tool& tool) {
  // Every parameter, described, and `required` listing only the ones that are.
  // This used to walk `required` for both, so an optional parameter could not
  // be expressed at all and no parameter carried a description — a model was
  // shown `capture_pane(target)` and left to guess whether `target` meant
  // `%1`, a session name, or `session:window.pane`.
  //
  // `additionalProperties: false` because a misspelt argument is a mistake
  // worth reporting rather than ignoring.
  json properties = json::object();
  for (const libtmux::mcp::Parameter& parameter : tool.parameters) {
    properties[parameter.name] =
        json{{"type", "string"}, {"description", parameter.description}};
  }
  return json{{"name", tool.name},
              {"description", tool.description},
              {"inputSchema",
               {{"type", "object"},
                {"properties", std::move(properties)},
                {"required", tool.required_names()},
                {"additionalProperties", false}}}};
}

// The MCP shape for a tool's answer: content blocks, plus a flag saying
// whether the tool itself failed. A tool failure is not a protocol error —
// the model is meant to read it and try something else.
json tool_result(std::string text, bool failed) {
  return json{
      {"content", json::array({json{{"type", "text"}, {"text", std::move(text)}}})},
      {"isError", failed}};
}

class Session {
public:
  explicit Session(libtmux::Server server)
      : server_{std::move(server)}, tools_{libtmux::mcp::default_tools()} {}

  [[nodiscard]] std::optional<json> handle(const json& request) {
    // Shape first. `value()` on a non-object throws, and a valid JSON array on
    // a line — `[1,2,3]` — parsed cleanly and then killed the process here,
    // where an `Invalid Request` was the whole answer required.
    if (!request.is_object()) {
      return failure(json{}, kInvalidRequest, "a request must be a JSON object");
    }

    // Read with `find` and a type check rather than `value`, which throws when
    // the key is present with the wrong type. A peer that sends
    // `{"jsonrpc": 2.0}` is making a mistake, not ending the conversation.
    const auto typed = [&request](const char* key, auto predicate) -> const json* {
      const auto found = request.find(key);
      return found != request.end() && predicate(*found) ? &*found : nullptr;
    };

    // JSON-RPC allows a string, a number or null, and nothing else. An id of
    // some other shape cannot be echoed back, so there is no reply that
    // correlates and the request is not one.
    const json* const identifier = typed("id", [](const json& value) {
      return value.is_string() || value.is_number() || value.is_null();
    });
    const bool is_notification = !request.contains("id");
    if (!is_notification && identifier == nullptr) {
      return failure(json{}, kInvalidRequest,
                     "an id must be a string, a number or null");
    }
    const json id = identifier == nullptr ? json{} : *identifier;

    const json* const version =
        typed("jsonrpc", [](const json& value) { return value.is_string(); });
    if (version == nullptr || version->get<std::string>() != "2.0") {
      return failure(id, kInvalidRequest, "not a JSON-RPC 2.0 request");
    }
    const json* const named =
        typed("method", [](const json& value) { return value.is_string(); });
    if (named == nullptr) {
      return failure(id, kInvalidRequest, "a request needs a method name");
    }
    const std::string method = named->get<std::string>();
    const json* const supplied =
        typed("params", [](const json& value) { return value.is_object(); });
    const json params = supplied == nullptr ? json::object() : *supplied;

    if (method == "initialize") {
      return success(id,
                     json{{"protocolVersion", kProtocolVersion},
                          {"capabilities", {{"tools", json::object()}}},
                          {"serverInfo",
                           {{"name", "libtmux"},
                            {"version", std::string{libtmux::library_version()}}}}});
    }
    if (method == "notifications/initialized" || method == "initialized") {
      // A notification carries no id and takes no reply.
      return std::nullopt;
    }
    if (method == "ping") {
      return success(id, json::object());
    }
    if (method == "tools/list") {
      json listed = json::array();
      for (const libtmux::mcp::Tool& tool : tools_.tools()) {
        listed.push_back(describe(tool));
      }
      return success(id, json{{"tools", std::move(listed)}});
    }
    if (method == "tools/call") {
      return call(id, params);
    }
    if (is_notification) {
      return std::nullopt;
    }
    return failure(id, kMethodNotFound, "no such method: " + method);
  }

private:
  [[nodiscard]] json call(const json& id, const json& params) {
    const auto name = params.find("name");
    if (name == params.end() || !name->is_string()) {
      return failure(id, kInvalidParams, "tools/call needs a tool name");
    }
    auto arguments = read_arguments(params);
    if (!arguments.has_value()) {
      return failure(id, kInvalidParams, arguments.error());
    }

    const auto answered = tools_.call(server_, name->get<std::string>(), *arguments);
    if (answered.has_value()) {
      return success(id, tool_result(*answered, false));
    }
    // A model supplying a bad argument and tmux refusing a well-formed request
    // are different things, and the tool surface already separates them: the
    // first is a protocol-level mistake, the second is a result the model
    // should read.
    if (answered.error().caller_error) {
      return failure(id, kInvalidParams, answered.error().message);
    }
    return success(id, tool_result(answered.error().message, true));
  }

  libtmux::Server server_;
  libtmux::mcp::ToolSet tools_;
};

libtmux::expected<libtmux::Server, std::string> open_server(int argc, char** argv) {
  // A socket path if given, the server this process is inside if not, and
  // otherwise the one tmux itself would use.
  if (argc > 1) {
    auto chosen = libtmux::Server::at_socket_path(argv[1]);
    if (!chosen.has_value()) {
      return libtmux::unexpected(chosen.error().diagnostic);
    }
    return *std::move(chosen);
  }
  if (auto inherited = libtmux::Server::from_env(); inherited.has_value()) {
    return *std::move(inherited);
  }
  auto fallback = libtmux::Server::at_default();
  if (!fallback.has_value()) {
    return libtmux::unexpected(fallback.error().diagnostic);
  }
  return *std::move(fallback);
}

} // namespace

int main(int argc, char** argv) {
  auto server = open_server(argc, argv);
  if (!server.has_value()) {
    std::fprintf(stderr, "libtmux-mcp: %s\n", server.error().c_str());
    return 1;
  }

  Session session{*std::move(server)};
  // Unbuffered enough to be a conversation: a client waits for each reply.
  std::ios::sync_with_stdio(false);

  // One request is one line, and a line is bounded. Without this a peer that
  // never sends a newline grows this string until the process is killed for
  // it, which is a denial of service written as an omission.
  constexpr std::string::size_type kMaximumLineBytes = 8U * 1024U * 1024U;

  for (std::string line; std::getline(std::cin, line);) {
    if (line.empty()) {
      continue;
    }
    if (line.size() > kMaximumLineBytes) {
      std::cout << failure(json{}, kInvalidRequest, "request line too long").dump()
                << '\n'
                << std::flush;
      continue;
    }
    // Every JSON error, not only the parse ones. `json::type_error` comes out
    // of ordinary reads, and catching the narrower type left it to escape
    // `main` and terminate a server that only had to answer "invalid request".
    try {
      const json request = json::parse(line);
      if (const auto reply = session.handle(request); reply.has_value()) {
        std::cout << reply->dump() << '\n' << std::flush;
      }
    } catch (const json::exception& error) {
      std::cout << failure(json{}, kParseError, error.what()).dump() << '\n'
                << std::flush;
    }
  }
  return 0;
}
