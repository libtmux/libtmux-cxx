"""The guards this port cannot afford to lose, and how to break each one.

Seeded from the mutations run by hand over the buffer, format, scripting and
key-binding work.  Each entry names a guard that exists because tmux does
something quiet: accepts a target it cannot resolve, takes a `-e` with no
`=`, prints a table name it cannot read back.

An entry that stops matching is a non-result, not a pass.  That is the point
of keeping them here rather than in a shell history.
"""

from __future__ import annotations

import typing as t

from .runner import Mutation

CATALOGUE: t.Final = (
    Mutation(
        mutation_id="expand-identity-guard",
        path="src/acquire.hpp",
        find="  if (!answer.starts_with(opening)) {",
        replace="  if (answer.size() == 987654321U) {",
        target="libtmux_backend_seam_test",
        test_regex=r"^libtmux[.]backend_seam$",
        guards="a format expanded against a target tmux cannot resolve is "
        "reported rather than answered with a blank",
    ),
    Mutation(
        mutation_id="expand-trailing-newline",
        path="src/acquire.hpp",
        find="  if (!text.empty() && text.back() == '\\n') {",
        replace="  while (!text.empty() && text.back() == '\\n') {",
        target="libtmux_backend_seam_test",
        test_regex=r"^libtmux[.]backend_seam$",
        guards="exactly one newline is removed, so a format that ends in one keeps it",
    ),
    Mutation(
        mutation_id="format-before-terminator",
        path="src/snapshot.cpp",
        find='    const auto terminator = std::ranges::find(request, "--");\n'
        '    request.insert(terminator, {"-F", format_request(fields)});',
        replace='    request.emplace_back("-F");\n'
        "    request.push_back(format_request(fields));",
        target="libtmux_backend_seam_test",
        test_regex=r"^libtmux[.]backend_seam$",
        guards="a creation call carrying a shell command still answers with a "
        "readable entity",
    ),
    Mutation(
        mutation_id="environment-name-check",
        path="src/acquire.hpp",
        find="    if (name.empty() || name.find('=') != std::string::npos) {",
        replace="    if (name.size() == 987654321U) {",
        target="libtmux_backend_seam_test",
        test_regex=r"^libtmux[.]backend_seam$",
        guards="a variable name tmux would take and silently ignore is refused",
    ),
    Mutation(
        mutation_id="key-table-whitespace",
        path="src/server.cpp",
        find="  if (table.find_first_of("
        '" \\t\\n\\r\\f\\v"'
        ") != std::string_view::npos) {",
        replace="  if (table.size() == 987654321U) {",
        target="libtmux_backend_seam_test",
        test_regex=r"^libtmux[.]backend_seam$",
        guards="a key table name that would make tmux's own listing ambiguous "
        "is refused",
    ),
    Mutation(
        mutation_id="control-batch-keeps-its-shape",
        path="src/control_backend.cpp",
        find="  for (const std::vector<std::string>& command : batch.commands()) {\n"
        "    request.group.push_back(ControlCommand{command});\n"
        "  }",
        replace="  request.group.push_back(ControlCommand{batch.argv()});",
        target="libtmux_backend_seam_test",
        test_regex=r"^libtmux[.]backend_seam$",
        guards="every command in a batch runs over a control connection, "
        "rather than the separator arriving escaped and the rest read as "
        "arguments to the first",
    ),
    Mutation(
        mutation_id="control-options-use-server-route",
        path="src/control_backend.cpp",
        find="  options.socket_path = std::move(socket_path);",
        replace="  static_cast<void>(socket_path);",
        target="libtmux_backend_seam_test",
        test_regex=r"^libtmux[.]backend_seam$",
        guards="caller control policy is preserved while the Server's socket "
        "remains the route used by the connection",
    ),
    Mutation(
        mutation_id="subprocess-capabilities-identify-tmux",
        path="src/backend.hpp",
        find="    return {.implementation = ServerImplementation::tmux,\n"
        "            .backend = BackendKind::subprocess};",
        replace="    return {.implementation = ServerImplementation::unknown,\n"
        "            .backend = BackendKind::subprocess};",
        target="libtmux_backend_seam_test",
        test_regex=r"^libtmux[.]backend_seam$",
        guards="a POSIX subprocess Server reports its local tmux capability "
        "contract without launching a process",
    ),
    Mutation(
        mutation_id="tmux37-unnamed-break-guard",
        path="src/entities.cpp",
        find='        "#{&&:#{==:#{version},3.7},#{>:#{window_panes},1}}",',
        replace='        "#{&&:#{==:#{version},3.7a},#{>:#{window_panes},1}}",',
        target="libtmux_backend_seam_test",
        test_regex=r"^libtmux[.]backend_seam$",
        guards="raw tmux 3.7 receives a name before an unnamed multi-pane "
        "break can crash its server",
    ),
    Mutation(
        mutation_id="tmux37-named-break-repair",
        path="src/entities.cpp",
        find=(
            "  const bool raw_tmux_37 = "
            "created->version == Version{.major = 3, .minor = 7};"
        ),
        replace=(
            "  const bool raw_tmux_37 = "
            "created->version == Version{.major = 3, .minor = 8};"
        ),
        target="libtmux_backend_seam_test",
        test_regex=r"^libtmux[.]backend_seam$",
        guards="raw tmux 3.7 repairs an ignored requested window name by its "
        "stable identity",
    ),
    Mutation(
        mutation_id="notification-bound",
        path="src/notification_stream.hpp",
        find="inline constexpr std::size_t kMaximumNotifications = 4096U;",
        replace="inline constexpr std::size_t kMaximumNotifications = 100000000U;",
        target="libtmux_backend_seam_test",
        test_regex=r"^libtmux[.]backend_seam$",
        guards="a caller that never drains gets a bounded buffer and a count "
        "of what was dropped",
    ),
    Mutation(
        mutation_id="legacy-empty-lookup",
        path="include/libtmux/legacy_lookup.hpp",
        find="    if (lookup.empty()) {\n"
        "      return unexpected(LookupParseError::unknown_lookup);\n"
        "    }",
        replace="",
        target="libtmux_legacy_lookup_test",
        test_regex=r"^libtmux[.]legacy[.]",
        guards="a key that asked for a lookup and named none is refused "
        "rather than read as equality",
    ),
    Mutation(
        mutation_id="mcp-batch-duplicate-preflight",
        path="apps/mcp/src/stdio_server.cpp",
        find="      if (occurrences.at(key) > 1U) {",
        replace="      if (occurrences.at(key) > requests.size()) {",
        target="mcp_protocol_test",
        executable="libtmux-mcp-server",
        test_regex=r"^consumer[.]mcp[.]protocol$",
        guards="a duplicate request ID rejects the whole legacy batch before any "
        "member can run",
    ),
    Mutation(
        mutation_id="mcp-id-held-through-write",
        path="apps/mcp/src/stdio_server.cpp",
        find="      if (response.has_value()) {\n"
        "        writer.send(*response);\n"
        "      }\n"
        "      dispatcher.release(reserved);",
        replace="      dispatcher.release(reserved);\n"
        "      if (response.has_value()) {\n"
        "        writer.send(*response);\n"
        "      }",
        target="mcp_protocol_test",
        executable="libtmux-mcp-server",
        test_regex=r"^consumer[.]mcp[.]protocol$",
        guards="a request ID stays reserved until its response is delivered",
    ),
    Mutation(
        mutation_id="mcp-wait-shares-one-deadline",
        path="apps/mcp/src/wait_for_text.cpp",
        find="  auto reply = server.run(command, *remaining);",
        replace=(
            "  auto reply = server.run(command, std::chrono::milliseconds{60000});"
        ),
        target="mcp_tools_test",
        test_regex=r"^consumer[.]mcp$",
        guards="lookup, capture, control setup, and polling share one wait deadline",
    ),
    Mutation(
        mutation_id="mcp-wait-omits-unresolved-pane",
        path="apps/mcp/src/wait_for_text.cpp",
        find="  if (!answer.pane_id.empty()) {",
        replace="  if (true) {",
        target="mcp_schema_test",
        test_regex=r"^consumer[.]mcp[.]schema$",
        guards="a wait that expired before resolving a target omits pane_id "
        "rather than publishing an empty one its schema refuses",
    ),
    Mutation(
        mutation_id="buffer-load-names-it",
        path="src/server.cpp",
        find='"load-buffer", "-b", std::string{name}, "--",',
        replace='"load-buffer", "--",',
        target="libtmux_backend_seam_test",
        test_regex=r"^libtmux[.]backend_seam$",
        guards="a file is loaded into the buffer the caller named",
    ),
    Mutation(
        mutation_id="windows-psmux-disables-warm-claiming",
        path="src/environment.hpp",
        find='      {"PSMUX_NO_WARM", "1"},',
        replace='      {"PSMUX_NO_WARM", "0"},',
        target="libtmux_windows_psmux_smoke",
        test_regex=r"^windows[.]psmux-smoke$",
        presets=("windows-psmux",),
        guards="every psmux child disables process warming before launch",
    ),
    Mutation(
        mutation_id="windows-psmux-clears-inherited-route",
        path="src/environment.hpp",
        find='      {"PSMUX_TARGET_SESSION", std::nullopt},',
        replace='      {"PSMUX_TARGET_SESSION", "outside"},',
        target="libtmux_windows_psmux_smoke",
        test_regex=r"^windows[.]psmux-smoke$",
        presets=("windows-psmux",),
        guards="a psmux child cannot inherit a caller's target session",
    ),
    Mutation(
        mutation_id="windows-psmux-rejects-rebound-snapshot",
        path="src/snapshot.cpp",
        find="      if (row[session_column] != session_id) {",
        replace="      if (row[session_column] == session_id) {",
        target="libtmux_windows_psmux_smoke",
        test_regex=r"^windows[.]psmux-smoke$",
        presets=("windows-psmux",),
        guards="a routed psmux snapshot cannot return rows from a replacement session",
    ),
    Mutation(
        mutation_id="windows-psmux-cleanup-reports-success",
        path="src/server.cpp",
        find="    found_any = true;",
        replace="    found_any = false;",
        target="libtmux_windows_psmux_smoke",
        test_regex=r"^windows[.]psmux-smoke$",
        presets=("windows-psmux",),
        guards="exact cleanup succeeds after removing its sessions while preserving a "
        "nested prefix",
    ),
)
