#pragma once

// An MCP tool surface over the library.
//
// This is the second consumer, and it pulls on the API from the opposite side
// to the workspace builder: every call arrives as untyped strings from a
// model, so it exercises validation and error reporting rather than
// composition.
//
// No JSON appears here. A tool takes named string arguments and returns text
// or a failure; encoding that into an MCP frame is the host's concern, which
// keeps this dependency-free the same way expression lowering is.

#include <expected>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "libtmux/server.hpp"

namespace libtmux::mcp {

using Arguments = std::map<std::string, std::string, std::less<>>;

struct ToolError {
  // A model supplying a bad argument is a different failure from tmux
  // refusing a well-formed request, and a host should surface them
  // differently.
  bool caller_error{};
  std::string message;
};

using ToolResult = libtmux::expected<std::string, ToolError>;
using Handler = std::function<ToolResult(const Server&, const Arguments&)>;

// One named argument, as a model sees it.
//
// `description` is the whole of what a model has to go on. A name alone does
// not say whether `target` takes `%1`, a session name, or `session:window.pane`
// — so each says what it accepts and shows one.
struct Parameter {
  std::string name;
  std::string description;
  bool required{true};
};

struct Tool {
  std::string name;
  std::string description;
  std::vector<Parameter> parameters;
  Handler handle;

  [[nodiscard]] std::vector<std::string> required_names() const {
    std::vector<std::string> names;
    for (const Parameter& parameter : parameters) {
      if (parameter.required) {
        names.push_back(parameter.name);
      }
    }
    return names;
  }
};

[[nodiscard]] inline const std::string* argument(const Arguments& arguments,
                                                 std::string_view name) {
  const auto found = arguments.find(name);
  return found == arguments.end() ? nullptr : &found->second;
}

class ToolSet {
public:
  void add(Tool tool) { tools_.push_back(std::move(tool)); }

  [[nodiscard]] const std::vector<Tool>& tools() const noexcept { return tools_; }

  // Dispatch by name, checking required arguments before reaching tmux so a
  // missing argument never becomes a confusing tmux message.
  [[nodiscard]] ToolResult call(const Server& server, std::string_view name,
                                const Arguments& arguments) const {
    for (const Tool& tool : tools_) {
      if (tool.name != name) {
        continue;
      }
      for (const Parameter& parameter : tool.parameters) {
        if (!parameter.required) {
          continue;
        }
        const std::string* value = argument(arguments, parameter.name);
        if (value == nullptr || value->empty()) {
          return libtmux::unexpected(
              ToolError{true, "missing required argument: " + parameter.name});
        }
      }
      return tool.handle(server, arguments);
    }
    return libtmux::unexpected(ToolError{true, "unknown tool: " + std::string{name}});
  }

private:
  std::vector<Tool> tools_;
};

// The default surface. Each tool is one library call, so a model cannot reach
// anything the library does not already expose.
[[nodiscard]] inline ToolSet default_tools() {
  ToolSet set;
  set.add(Tool{.name = "list_sessions",
               .description = "List every session on the server, one name per "
                              "line. Names are what `target` arguments accept "
                              "wherever a session is asked for.",
               .parameters = {},
               .handle = [](const Server& server, const Arguments&) -> ToolResult {
                 const auto sessions = server.sessions();
                 if (!sessions.has_value()) {
                   return libtmux::unexpected(
                       ToolError{false, sessions.error().diagnostic});
                 }
                 std::string out;
                 for (const Session& session : *sessions) {
                   out += session.name();
                   out += '\n';
                 }
                 return out;
               }});
  set.add(Tool{.name = "list_panes",
               .description = "List every pane as three tab-separated columns: "
                              "pane id, window id, and the command running in "
                              "it. The pane id is what `target` accepts.",
               .parameters = {},
               .handle = [](const Server& server, const Arguments&) -> ToolResult {
                 const auto panes = server.panes();
                 if (!panes.has_value()) {
                   return libtmux::unexpected(
                       ToolError{false, panes.error().diagnostic});
                 }
                 std::string out;
                 for (const Pane& pane : *panes) {
                   out += pane.id();
                   out += '\t';
                   out += pane.window_id();
                   out += '\t';
                   out += pane.command();
                   out += '\n';
                 }
                 return out;
               }});
  set.add(Tool{
      .name = "capture_pane",
      .description = "Return the visible contents of one pane, as text.",
      .parameters = {{.name = "target",
                      .description = "Which pane. A pane id such as `%1`, or "
                                     "anything tmux resolves to one: a session "
                                     "name, `session:window`, or "
                                     "`session:window.pane`. `list_panes` "
                                     "returns ids."}},
      .handle = [](const Server& server, const Arguments& arguments) -> ToolResult {
        // Resolving the target first is what turns "there is no such pane"
        // into a distinct answer instead of empty output.
        const auto pane = server.pane(*argument(arguments, "target"));
        if (!pane.has_value()) {
          return libtmux::unexpected(ToolError{false, pane.error().diagnostic});
        }
        const auto captured = pane->capture();
        if (!captured.has_value()) {
          return libtmux::unexpected(ToolError{false, captured.error().diagnostic});
        }
        return *captured;
      }});
  set.add(Tool{
      .name = "send_text",
      .description = "Type literal text into one pane. Nothing is interpreted: "
                     "key names like `C-c` and `Enter` arrive as those "
                     "characters, not as keys. Returns the pane id written to.",
      .parameters = {{.name = "target",
                      .description = "Which pane. A pane id such as `%1`, or "
                                     "anything tmux resolves to one."},
                     {.name = "text",
                      .description = "The literal characters to type. Include a "
                                     "trailing newline to submit a line."}},
      .handle = [](const Server& server, const Arguments& arguments) -> ToolResult {
        const auto pane = server.pane(*argument(arguments, "target"));
        if (!pane.has_value()) {
          return libtmux::unexpected(ToolError{false, pane.error().diagnostic});
        }
        const auto sent = pane->send_text(*argument(arguments, "text"));
        if (!sent.has_value()) {
          return libtmux::unexpected(ToolError{false, sent.error().diagnostic});
        }
        return std::string{pane->id()};
      }});
  set.add(Tool{
      .name = "new_window",
      .description = "Create a window in one session without switching to it. "
                     "Returns the new window id, such as `@3`.",
      .parameters = {{.name = "session",
                      .description = "Which session, by name as `list_sessions` "
                                     "reports it, or a session id such as `$0`."},
                     {.name = "name",
                      .description = "The window name, as it appears in the "
                                     "status line."}},
      .handle = [](const Server& server, const Arguments& arguments) -> ToolResult {
        const auto session = server.session(*argument(arguments, "session"));
        if (!session.has_value()) {
          return libtmux::unexpected(ToolError{false, session.error().diagnostic});
        }
        const auto window = session->new_window(*argument(arguments, "name"));
        if (!window.has_value()) {
          return libtmux::unexpected(ToolError{false, window.error().diagnostic});
        }
        return std::string{window->id()};
      }});
  return set;
}

} // namespace libtmux::mcp
