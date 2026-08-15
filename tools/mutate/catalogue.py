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
        path="cxx/src/acquire.hpp",
        find="  if (!answer.starts_with(opening)) {",
        replace="  if (answer.size() == 987654321U) {",
        target="libtmux_expand_test",
        guards="a format expanded against a target tmux cannot resolve is "
        "reported rather than answered with a blank",
    ),
    Mutation(
        mutation_id="expand-trailing-newline",
        path="cxx/src/acquire.hpp",
        find="  if (!text.empty() && text.back() == '\\n') {",
        replace="  while (!text.empty() && text.back() == '\\n') {",
        target="libtmux_expand_test",
        guards="exactly one newline is removed, so a format that ends in one keeps it",
    ),
    Mutation(
        mutation_id="format-before-terminator",
        path="cxx/src/snapshot.cpp",
        find='    const auto terminator = std::ranges::find(request, "--");\n'
        '    request.insert(terminator, {"-F", format_request(fields)});',
        replace='    request.emplace_back("-F");\n'
        "    request.push_back(format_request(fields));",
        target="libtmux_environment_test",
        guards="a creation call carrying a shell command still answers with a "
        "readable entity",
    ),
    Mutation(
        mutation_id="environment-name-check",
        path="cxx/src/acquire.hpp",
        find="    if (name.empty() || name.find('=') != std::string::npos) {",
        replace="    if (name.size() == 987654321U) {",
        target="libtmux_environment_test",
        guards="a variable name tmux would take and silently ignore is refused",
    ),
    Mutation(
        mutation_id="key-table-whitespace",
        path="cxx/src/server.cpp",
        find="  if (table.find_first_of("
        '" \\t\\n\\r\\f\\v"'
        ") != std::string_view::npos) {",
        replace="  if (table.size() == 987654321U) {",
        target="libtmux_key_binding_test",
        guards="a key table name that would make tmux's own listing ambiguous "
        "is refused",
    ),
    Mutation(
        mutation_id="control-batch-keeps-its-shape",
        path="cxx/src/control_backend.cpp",
        find="  for (const std::vector<std::string>& command : batch.commands()) {\n"
        "    request.group.push_back(ControlCommand{command});\n"
        "  }",
        replace="  request.group.push_back(ControlCommand{batch.argv()});",
        target="libtmux_control_dispatch_test",
        guards="every command in a batch runs over a control connection, "
        "rather than the separator arriving escaped and the rest read as "
        "arguments to the first",
    ),
    Mutation(
        mutation_id="notification-bound",
        path="cxx/src/connection.cpp",
        find="constexpr std::size_t maximum_notifications = 4096U;",
        replace="constexpr std::size_t maximum_notifications = 100000000U;",
        target="libtmux_control_dispatch_test",
        guards="a caller that never drains gets a bounded buffer and a count "
        "of what was dropped",
    ),
    Mutation(
        mutation_id="legacy-empty-lookup",
        path="cxx/include/libtmux/legacy_lookup.hpp",
        find="    if (lookup.empty()) {\n"
        "      return unexpected(LookupParseError::unknown_lookup);\n"
        "    }",
        replace="",
        target="libtmux_legacy_lookup_test",
        guards="a key that asked for a lookup and named none is refused "
        "rather than read as equality",
    ),
    Mutation(
        mutation_id="buffer-load-names-it",
        path="cxx/src/server.cpp",
        find='"load-buffer", "-b", std::string{name}, "--", from.string()',
        replace='"load-buffer", "--", from.string()',
        target="libtmux_buffer_test",
        guards="a file is loaded into the buffer the caller named",
    ),
)
