# MCP server guidance

Follow the repository-root `AGENTS.md` and `.github/WRITING.md` first.

The MCP is a curated, semantic, detached-safe interface. Library parity does
not require MCP parity. Keep modal human-client UI out when capture or another
noninteractive operation provides the result: copy mode, clock mode,
choose-tree, prompts, menus, popups, and mouse gestures are exclusion signals.

Use capture, retained history, snapshots, search, and `capture_since` for pane
text. Report mode state when needed; do not enter or cancel it through the MCP.
Paired enter/exit cleanup, unclear ownership, or dependence on key tables,
mouse state, clipboard state, or timing also argues against a public tool.
Keep the core libtmux API even when the MCP omits that command.

Every public tool belongs to one ADR toolset. The authoritative manifest owns
runtime registration, documentation, and tests.
