# MCP configuration switcher

Use `mcp-swap` when every installed agent CLI should run a particular build of
`libtmux-mcp-server`: one from this checkout's build tree or one already
installed. `use-local` rewrites each selected CLI's configuration to run that
binary by absolute path; `revert` restores the exact bytes and mode from the
authenticated first backup. Repeating a swap keeps that first backup, so
`revert` always returns to the pre-swap configuration.

The server registers as `libtmux`, and the binary name comes from the
`OUTPUT_NAME` on its CMake target, so a rename remains defined in one place.
This repository-only executable is built with the test tooling; it is not
installed, exported, or packaged.

## Build

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target mcp_swap
```

The examples below use the `cxx-dev` output. Substitute another configured
preset's build directory when needed.

## Examples

```console
$ ./build/cxx-dev/tools/mcp/mcp-swap detect
```

```console
$ ./build/cxx-dev/tools/mcp/mcp-swap status
```

```console
$ ./build/cxx-dev/tools/mcp/mcp-swap use-local \
    --dry-run \
    --socket /tmp/libtmux-agent/socket
```

```console
$ ./build/cxx-dev/tools/mcp/mcp-swap use-local \
    --build-dir build/cxx-gcc \
    --socket /tmp/libtmux-agent/socket
```

```console
$ ./build/cxx-dev/tools/mcp/mcp-swap use-local \
    --source published \
    --socket /tmp/libtmux-agent/socket
```

```console
$ ./build/cxx-dev/tools/mcp/mcp-swap revert
```

`use-local --dry-run` parses and authenticates every selected configuration and
prints the proposed diffs without starting the server or writing recovery
state. A real swap preflights each client's final command and merged environment
before changing any configuration.

Existing per-entry environment values are retained. Explicit
`--env LIBTMUX_SAFETY=...` is refused. An inherited `LIBTMUX_SAFETY` is removed
only when the request explicitly supplies `LIBTMUX_TOOLSETS`; otherwise it
stays in the proposed definition so preflight can report the required migration.

## Scope

The tool is intentionally narrow:

- **POSIX client configs only.** The Windows psmux setup carries a task-owned
  config and high-entropy `--socket-name` through a PowerShell wrapper. This
  tool neither models nor rewrites that wrapper.

- **Global configs only.** It writes `~/.cursor/mcp.json`, `~/.claude.json`,
  `~/.codex/config.toml`, `~/.gemini/settings.json`,
  `~/.grok/config.toml` (TOML `mcp_servers`, the same shape as Codex),
  `~/.gemini/config/mcp_config.json` (agy / Antigravity CLI, JSON `mcpServers`;
  this is the shared-config file the CLI reads, beside the `config.json` it
  loads at startup),
  `$XDG_CONFIG_HOME/opencode/opencode.jsonc` (JSONC `mcp`, comments preserved),
  and `~/.pi/agent/mcp.json` (JSONC whose comments the adapter strips).
  Workspace or project-local configs such as `$PWD/.cursor/mcp.json`,
  `$PWD/.gemini/settings.json`, and `$PWD/opencode.json` are not walked and are
  silently ignored. The exception is Claude's
  `projects.<main-worktree>.mcpServers` entry inside `~/.claude.json`. When
  workspace precedence matters, use the CLI's own `cursor mcp add` or
  `gemini mcp add` command. opencode has no non-interactive project-scope add;
  `opencode mcp add` writes the global file, so edit `$PWD/opencode.json`
  directly when that is the intended layer.

- **opencode reads three global files.** `config.json`, `opencode.json`, and
  `opencode.jsonc` in the same directory are merged with `.jsonc` winning. This
  tool owns `.jsonc`, the file opencode itself writes. A stale `mcp.<name>` in a
  sibling `opencode.json` still merges underneath; remove it manually if that
  matters.

- **pi has no MCP client of its own.** Its released build ships no MCP code.
  `~/.pi/agent/mcp.json` is read by the third-party `pi-mcp-adapter` extension.
  A swap there takes effect only after that package is installed, and `detect`
  reports the missing adapter rather than claiming a usable integration.

- **Claude scope.** `use-local` and `revert` accept
  `--scope user|project`. The default `project` writes the per-project entry
  under `projects[<main-worktree>].mcpServers`; only that repository sees the
  swap, matching the earlier behavior. `--scope user` writes Claude's top-level
  `mcpServers` fallback, so every project without its own override sees the
  swap; this is useful when testing a branch across many directories. Other
  clients have no per-project layer in the files this tool writes and silently
  coerce the flag to `user`. Both Claude scopes can coexist with independent
  backups, and a full `revert` unwinds them in physical LIFO order.

- **Simple binary detection.** Detection checks the named binary on `PATH` and
  whether its configuration file exists. Custom Homebrew or npm prefixes,
  `~/.npm-global/bin`, `~/.claude/local/claude`, and
  `~/.gemini/local/gemini` are found only when they are on `PATH`; unlike
  FastMCP's installer, this tool does not probe those locations directly.

- **Single config shape per CLI.** There are no fallback paths or models of
  client-specific multi-file merging. If a setup differs from these defaults,
  use that CLI's native `mcp` subcommand.

## Recovery boundary

Only one process may transact at a time; a later process waits on the shared
POSIX record lock until the holder releases it. Before every publication
boundary, the tool reauthenticates the state file, selected and unselected
configuration routes, prior recovery backups, modes, and physical identities.
Recovery files must be private, owned regular files with one hard link. If
rollback cannot prove that a destination is still the one it planned, it leaves
checksummed pending state and backups in place instead of overwriting an
external edit. The cross-port lock is
`$XDG_STATE_HOME/libtmux-mcp-dev/swap/state.lock`; C++ recovery state lives
under the adjacent `cxx/` directory, and its config backups use the
`.bak.mcp-swap-cxx-` marker. Other language ports do not share those native
recovery records.

## Related

- [MCP server](../../apps/mcp/README.md)
- [Development tools](../README.md)
