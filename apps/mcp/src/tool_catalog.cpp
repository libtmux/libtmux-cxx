#include "libtmux_consumers/mcp.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "libtmux/chain.hpp"
#include "libtmux/keys.hpp"
#include "libtmux/server.hpp"
#include "tool_support.hpp"
#include "wait_for_text.hpp"

namespace libtmux::mcp::detail {

const std::string* argument(const Arguments& arguments, std::string_view name) {
  const auto found = arguments.find(name);
  return found == arguments.end() ? nullptr : &found->second;
}

ToolOutput output(StructuredValue::Object structured) {
  return ToolOutput{.structured = std::move(structured)};
}

ToolError tmux_error(const CommandFailure& error) {
  return ToolError{false, error.diagnostic};
}

StructuredValue session_value(const Session& session) {
  return StructuredValue::Object{
      {"attached", StructuredValue{session.attached()}},
      {"id", StructuredValue{session.id()}},
      {"name", StructuredValue{session.name()}},
      {"window_count", StructuredValue{session.window_count()}},
  };
}

StructuredValue window_value(const Window& window) {
  return StructuredValue::Object{
      {"active", StructuredValue{window.active()}},
      {"id", StructuredValue{window.id()}},
      {"index", StructuredValue{window.index()}},
      {"name", StructuredValue{window.name()}},
      {"session_id", StructuredValue{window.session_id()}},
  };
}

StructuredValue pane_value(const Pane& pane) {
  return StructuredValue::Object{
      {"active", StructuredValue{pane.active()}},
      {"command", StructuredValue{pane.command()}},
      {"id", StructuredValue{pane.id()}},
      {"session_id", StructuredValue{pane.session_id()}},
      {"window_id", StructuredValue{pane.window_id()}},
  };
}

namespace {

constexpr ToolAnnotations kReadOnly{
    .read_only = true, .destructive = false, .idempotent = true, .open_world = false};
constexpr ToolAnnotations kTerminalInput{
    .read_only = false, .destructive = true, .idempotent = false, .open_world = true};
constexpr ToolAnnotations kAdditive{
    .read_only = false, .destructive = false, .idempotent = false, .open_world = false};

[[nodiscard]] ToolError cancelled() { return ToolError{false, "request cancelled"}; }

} // namespace
} // namespace libtmux::mcp::detail

namespace libtmux::mcp {

ToolSet default_tools() {
  ToolSet set;
  set.add(Tool{
      .name = "inspect_tmux",
      .title = "Inspect tmux",
      .description =
          "Inspect sessions, their windows, and their panes in one typed result. "
          "Start here and retain every owning session ID.",
      .parameters = {},
      .output = OutputShape::overview,
      .annotations = detail::kReadOnly,
      .handle = [](const Server& server, const Arguments&,
                   const CallContext& context) -> ToolResult {
        const auto sessions = server.sessions();
        if (!sessions.has_value()) {
          return libtmux::unexpected(detail::tmux_error(sessions.error()));
        }
        StructuredValue::Array session_rows;
        StructuredValue::Array window_rows;
        StructuredValue::Array pane_rows;
        for (const Session& session : *sessions) {
          if (context.cancelled()) {
            return libtmux::unexpected(detail::cancelled());
          }
          session_rows.push_back(detail::session_value(session));
          const auto windows = session.windows();
          if (!windows.has_value()) {
            return libtmux::unexpected(detail::tmux_error(windows.error()));
          }
          for (const Window& window : *windows) {
            window_rows.push_back(detail::window_value(window));
          }
          const auto panes = session.panes();
          if (!panes.has_value()) {
            return libtmux::unexpected(detail::tmux_error(panes.error()));
          }
          for (const Pane& pane : *panes) {
            pane_rows.push_back(detail::pane_value(pane));
          }
        }

        return detail::output({{"panes", StructuredValue{std::move(pane_rows)}},
                               {"sessions", StructuredValue{std::move(session_rows)}},
                               {"windows", StructuredValue{std::move(window_rows)}}});
      }});
  set.add(
      Tool{.name = "list_sessions",
           .title = "List tmux sessions",
           .description = "List each session as stable id and name. Prefer the id in "
                          "later calls because names can change.",
           .parameters = {},
           .output = OutputShape::sessions,
           .annotations = detail::kReadOnly,
           .handle = [](const Server& server, const Arguments&,
                        const CallContext&) -> ToolResult {
             const auto sessions = server.sessions();
             if (!sessions.has_value()) {
               return libtmux::unexpected(detail::tmux_error(sessions.error()));
             }
             StructuredValue::Array rows;
             for (const Session& session : *sessions) {
               rows.push_back(detail::session_value(session));
             }
             return detail::output({{"sessions", StructuredValue{std::move(rows)}}});
           }});
  set.add(Tool{
      .name = "list_windows",
      .title = "List windows in a session",
      .description = "List windows through an exact owning session. This is the "
                     "safe window discovery path on psmux.",
      .parameters = {{.name = "session",
                      .description = "Stable session ID such as `$0`, or its name.",
                      .maximum_length = detail::kTargetCharacters}},
      .output = OutputShape::windows,
      .annotations = detail::kReadOnly,
      .handle = [](const Server& server, const Arguments& arguments,
                   const CallContext&) -> ToolResult {
        const auto session = server.session(*detail::argument(arguments, "session"));
        if (!session.has_value()) {
          return libtmux::unexpected(detail::tmux_error(session.error()));
        }
        const auto windows = session->windows();
        if (!windows.has_value()) {
          return libtmux::unexpected(detail::tmux_error(windows.error()));
        }
        StructuredValue::Array rows;
        for (const Window& window : *windows) {
          rows.push_back(detail::window_value(window));
        }
        return detail::output({{"windows", StructuredValue{std::move(rows)}}});
      }});
  set.add(Tool{
      .name = "list_session_panes",
      .title = "List panes in a session",
      .description = "List panes through an exact owning session. Pane IDs repeat "
                     "between psmux sessions, so keep the returned session ID.",
      .parameters = {{.name = "session",
                      .description = "Stable session ID such as `$0`, or its name.",
                      .maximum_length = detail::kTargetCharacters}},
      .output = OutputShape::panes,
      .annotations = detail::kReadOnly,
      .handle = [](const Server& server, const Arguments& arguments,
                   const CallContext&) -> ToolResult {
        const auto session = server.session(*detail::argument(arguments, "session"));
        if (!session.has_value()) {
          return libtmux::unexpected(detail::tmux_error(session.error()));
        }
        const auto panes = session->panes();
        if (!panes.has_value()) {
          return libtmux::unexpected(detail::tmux_error(panes.error()));
        }
        StructuredValue::Array rows;
        for (const Pane& pane : *panes) {
          rows.push_back(detail::pane_value(pane));
        }
        return detail::output({{"panes", StructuredValue{std::move(rows)}}});
      }});

  // psmux 3.3.7 can report another process's same-name mutation as its own.
  // Keep the Windows MCP catalog read-only until ownership can be proven.
#if !defined(_WIN32)
  set.add(Tool{
      .name = "create_session",
      .title = "Create a tmux session",
      .description = "Create a detached session and return its stable ID and name.",
      .parameters = {{.name = "name",
                      .description = "Unique session name.",
                      .maximum_length = detail::kTargetCharacters}},
      .output = OutputShape::session_id,
      .annotations = detail::kAdditive,
      .handle = [](const Server& server, const Arguments& arguments,
                   const CallContext&) -> ToolResult {
        const auto session = server.new_session(*detail::argument(arguments, "name"));
        if (!session.has_value()) {
          return libtmux::unexpected(detail::tmux_error(session.error()));
        }
        const std::string id{session->id()};
        return detail::output({{"name", StructuredValue{session->name()}},
                               {"session_id", StructuredValue{id}}});
      }});
  set.add(Tool{
      .name = "new_window",
      .title = "Create a tmux window",
      .description =
          "Create a detached window through an exact owning session and return "
          "stable window and session IDs.",
      .parameters = {{.name = "session",
                      .description = "Stable session ID such as `$0`, or its name.",
                      .maximum_length = detail::kTargetCharacters},
                     {.name = "name",
                      .description = "Name for the new window.",
                      .maximum_length = detail::kTargetCharacters}},
      .output = OutputShape::window_id,
      .annotations = detail::kAdditive,
      .handle = [](const Server& server, const Arguments& arguments,
                   const CallContext&) -> ToolResult {
        const auto session = server.session(*detail::argument(arguments, "session"));
        if (!session.has_value()) {
          return libtmux::unexpected(detail::tmux_error(session.error()));
        }
        const auto window = session->new_window(*detail::argument(arguments, "name"));
        if (!window.has_value()) {
          return libtmux::unexpected(detail::tmux_error(window.error()));
        }
        const std::string id{window->id()};
        return detail::output({{"session_id", StructuredValue{window->session_id()}},
                               {"window_id", StructuredValue{id}}});
      }});
  set.add(Tool{.name = "list_panes",
               .title = "List all tmux panes",
               .description =
                   "List every pane with stable pane, window, and session IDs plus the "
                   "running command. Prefer pane IDs as targets.",
               .parameters = {},
               .output = OutputShape::panes,
               .annotations = detail::kReadOnly,
               .handle = [](const Server& server, const Arguments&,
                            const CallContext&) -> ToolResult {
                 const auto panes = server.panes();
                 if (!panes.has_value()) {
                   return libtmux::unexpected(detail::tmux_error(panes.error()));
                 }
                 StructuredValue::Array rows;
                 for (const Pane& pane : *panes) {
                   rows.push_back(detail::pane_value(pane));
                 }
                 return detail::output({{"panes", StructuredValue{std::move(rows)}}});
               }});
  set.add(Tool{
      .name = "capture_pane",
      .title = "Capture a tmux pane",
      .description = "Return the visible rendered text of one pane. Use "
                     "wait_for_text when waiting for future output.",
      .parameters = {{.name = "target",
                      .description = "Pane ID such as `%1`, or a tmux pane target.",
                      .maximum_length = detail::kTargetCharacters}},
      .output = OutputShape::pane_text,
      .annotations = detail::kReadOnly,
      .handle = [](const Server& server, const Arguments& arguments,
                   const CallContext&) -> ToolResult {
        const auto pane = server.pane(*detail::argument(arguments, "target"));
        if (!pane.has_value()) {
          return libtmux::unexpected(detail::tmux_error(pane.error()));
        }
        const auto captured = pane->capture();
        if (!captured.has_value()) {
          return libtmux::unexpected(detail::tmux_error(captured.error()));
        }
        return detail::output({{"pane_id", StructuredValue{pane->id()}},
                               {"text", StructuredValue{*captured}}});
      }});
  set.add(Tool{
      .name = "send_text",
      .title = "Type literal pane text",
      .description = "Type literal characters into one pane. Key names are not "
                     "interpreted; include a newline to submit a shell line.",
      .parameters = {{.name = "target",
                      .description = "Pane ID such as `%1`, or a tmux pane target.",
                      .maximum_length = detail::kTargetCharacters},
                     {.name = "text",
                      .description = "Literal text to type, including any newline.",
                      .maximum_length = 1024U * 1024U}},
      .output = OutputShape::pane_id,
      .annotations = detail::kTerminalInput,
      .handle = [](const Server& server, const Arguments& arguments,
                   const CallContext&) -> ToolResult {
        const auto pane = server.pane(*detail::argument(arguments, "target"));
        if (!pane.has_value()) {
          return libtmux::unexpected(detail::tmux_error(pane.error()));
        }
        const auto sent = pane->send_text(*detail::argument(arguments, "text"));
        if (!sent.has_value()) {
          return libtmux::unexpected(detail::tmux_error(sent.error()));
        }
        const std::string id{pane->id()};
        return detail::output({{"pane_id", StructuredValue{id}}});
      }});
  set.add(Tool{
      .name = "send_keys",
      .title = "Press named tmux keys",
      .description = "Press validated tmux key names such as `Enter` or `C-c`. "
                     "Use send_text for literal characters.",
      .parameters = {{.name = "target",
                      .description = "Pane ID such as `%1`, or a tmux pane target.",
                      .maximum_length = detail::kTargetCharacters},
                     {.name = "keys",
                      .description = "Space-separated tmux key names. Every key is "
                                     "validated before any is sent.",
                      .maximum_length = detail::kSearchCharacters}},
      .output = OutputShape::pane_id,
      .annotations = detail::kTerminalInput,
      .handle = [](const Server& server, const Arguments& arguments,
                   const CallContext& context) -> ToolResult {
        const auto pane = server.pane(*detail::argument(arguments, "target"));
        if (!pane.has_value()) {
          return libtmux::unexpected(detail::tmux_error(pane.error()));
        }
        std::vector<std::string> keys;
        const std::string& text = *detail::argument(arguments, "keys");
        std::size_t index = 0;
        while (index < text.size()) {
          const auto space = text.find(' ', index);
          const auto key = text.substr(
              index, space == std::string::npos ? std::string::npos : space - index);
          if (!key.empty()) {
            if (!libtmux::is_key_name(key)) {
              return libtmux::unexpected(ToolError{true, "not a key name: " + key});
            }
            keys.push_back(key);
          }
          if (space == std::string::npos) {
            break;
          }
          index = space + 1U;
        }
        if (keys.empty()) {
          return libtmux::unexpected(ToolError{true, "no keys given"});
        }
        Chain chain;
        for (const std::string& key : keys) {
          chain.send_key(pane->id(), key);
        }
        if (!chain.valid()) {
          return libtmux::unexpected(ToolError{true, chain.error()});
        }
        if (context.cancelled()) {
          return libtmux::unexpected(detail::cancelled());
        }
        if (auto sent = server.run_chain(chain); !sent.has_value()) {
          return libtmux::unexpected(detail::tmux_error(sent.error()));
        }
        const std::string id{pane->id()};
        return detail::output({{"pane_id", StructuredValue{id}}});
      }});
  set.add(Tool{
      .name = "wait_for_text",
      .title = "Wait for pane text",
      .description =
          "Wait for text without blocking other MCP requests. Uses "
          "tmux control-output events when available and bounded capture polling "
          "otherwise.",
      .parameters = {{.name = "target",
                      .description = "Pane ID such as `%1`, or a tmux pane target.",
                      .maximum_length = detail::kTargetCharacters},
                     {.name = "text",
                      .description = "Non-empty substring to wait for.",
                      .maximum_length = detail::kSearchCharacters},
                     {.name = "timeout_ms",
                      .description = "Timeout in milliseconds, from 1 through 60000. "
                                     "Defaults to 10000.",
                      .type = ArgumentType::integer,
                      .required = false,
                      .minimum = 1,
                      .maximum = 60000}},
      .output = OutputShape::wait,
      .annotations = detail::kReadOnly,
      .handle = detail::wait_for_text});
  set.add(Tool{
      .name = "search_panes",
      .title = "Search visible pane text",
      .description =
          "Find panes whose visible rendered text contains a substring. Returns "
          "stable pane IDs and the matching line.",
      .parameters = {{.name = "text",
                      .description = "Non-empty substring to search for.",
                      .maximum_length = detail::kSearchCharacters}},
      .output = OutputShape::matches,
      .annotations = detail::kReadOnly,
      .handle = [](const Server& server, const Arguments& arguments,
                   const CallContext& context) -> ToolResult {
        const auto panes = server.panes();
        if (!panes.has_value()) {
          return libtmux::unexpected(detail::tmux_error(panes.error()));
        }
        const std::string& wanted = *detail::argument(arguments, "text");
        StructuredValue::Array matches;
        for (const Pane& pane : *panes) {
          if (context.cancelled()) {
            return libtmux::unexpected(detail::cancelled());
          }
          const auto captured = pane.capture();
          if (!captured.has_value()) {
            return libtmux::unexpected(detail::tmux_error(captured.error()));
          }
          const auto found = captured->find(wanted);
          if (found == std::string::npos) {
            continue;
          }
          const auto line_start = captured->rfind('\n', found);
          const auto begin = line_start == std::string::npos ? 0U : line_start + 1U;
          const auto line_end = captured->find('\n', found);
          const std::string line =
              captured->substr(begin, line_end == std::string::npos ? std::string::npos
                                                                    : line_end - begin);
          matches.push_back(StructuredValue::Object{
              {"line", StructuredValue{line}},
              {"pane_id", StructuredValue{pane.id()}},
          });
        }
        return detail::output({{"matches", StructuredValue{std::move(matches)}}});
      }});
#endif
  return set;
}

} // namespace libtmux::mcp
