# libtmux-mcp-server

A [Model Context Protocol](https://modelcontextprotocol.io) server for
[tmux](https://github.com/tmux/tmux), built on the C++
[libtmux](../../README.md). It speaks newline-delimited JSON-RPC over stdio and
ships as one native executable.

This is an alpha interface. The same startup-frozen 45-tool manifest is
advertised on POSIX and native Windows. Psmux support remains
command-dependent on Windows: unsupported operations fail explicitly at
dispatch rather than disappearing from the declared capability surface.

## Tool catalog

One native registry owns the 45 public tools and their names, titles,
controlled descriptions, toolsets, capability claims, conservative
annotations, schemas, internal input-sink tables, nested authority, and
handlers. Its startup-filtered definitions govern both listing and calls:

Input sinks remain internal manifest and CI data. Public capability rows expose
only schema-keyed `inputLiteralization`, never `inputSinks` or
`tmuxFormatControls`.

<!-- BEGIN GENERATED TOOL INVENTORY -->
<!-- Generated from tmux://capabilities; run this script to update. -->
| Toolset | Tools |
|---|---|
| `inspect` | `list_sessions`, `list_windows`, `list_panes`, `get_server_info`, `get_session_info`, `get_window_info`, `get_pane_info`, `capture_pane`, `capture_since`, `snapshot_pane`, `search_panes`, `find_pane_by_position`, `wait_for_text`, `get_tmux_variables`, `show_option`, `show_environment`, `show_hooks`, `call_read_tools_batch` |
| `manage` | `rename_session`, `rename_window`, `select_window`, `select_pane`, `select_layout`, `resize_window`, `resize_pane`, `move_window`, `swap_pane`, `set_pane_title`, `wait_for_channel`, `signal_channel`, `set_mouse_enabled`, `set_history_limit` |
| `execute` | `create_session`, `create_window`, `split_window`, `respawn_pane`, `run_shell_command`, `send_keys`, `send_keys_batch`, `paste_text`, `set_synchronize_panes` |
| `teardown` | `clear_pane_scrollback`, `kill_pane`, `kill_window`, `kill_session` |
<!-- END GENERATED TOOL INVENTORY -->

### Moving from the twelve-tool alpha

Eight route names remain, though callers must use their current schemas. The
other four map as follows:

| Earlier route | Current route |
|---|---|
| `inspect_tmux` | `list_sessions` + `list_windows` + `list_panes` |
| `list_session_panes` | `list_panes`; filter returned `session_id` |
| `new_window` | `create_window`; `session` and `name` remain |
| `send_text` | `paste_text`; `target` becomes `paneId` |

### Core workflow quick reference

Start with server and object discovery, then retain the stable IDs returned by
those calls. These common workflows keep their argument and result guidance
beside the full generated inventory:

| Tool | Arguments | Result |
|---|---|---|
| `get_server_info` | — | Server liveness and tmux version |
| `list_sessions` | — | Stable session IDs, names, paths, client counts, and window counts |
| `list_windows` | `session` | Windows belonging to one exact session |
| `list_panes` | — | Every pane with stable pane, window, and session IDs |
| `create_session` | `name?`, `windowName?`, `startDirectory?`, `width?`, `height?` | New session ID and name |
| `create_window` | `session`, `name?`, `startDirectory?` | New window ID and owning session ID |
| `capture_pane` | `paneId`, `history?` | Visible or retained pane text |
| `capture_since` | `paneId`, `cursor?` | Text after a byte cursor and a replacement cursor |
| `snapshot_pane` | `paneId` | Pane metadata and visible text from one call |
| `paste_text` | `paneId`, `text` | Pane ID after pasting literal text |
| `send_keys` | `paneId`, `keys` | Pane ID and every resolved synchronized target |
| `wait_for_text` | `target`, `text`, `timeout_ms?` | Match, timeout, elapsed time, transport mode, and final capture |
| `search_panes` | `pattern` | Matching pane IDs and matching lines |
| `call_read_tools_batch` | `operations`, `onError?` | Ordered nested results, counters, and explicit truncation state |

Read terminal text without changing pane modes. `capture_pane` with `history`
retrieves retained scrollback, `capture_since` follows appended output,
`snapshot_pane` pairs visible text with metadata, and `search_panes` finds
matching lines across panes. Copy mode remains in the core C++ API for
human-client integrations; the MCP omits its modal enter and exit operations.

Creation and respawn tools accept no command or environment payload.
`run_shell_command` runs an explicit shell command in a pane. `send_keys`,
`send_keys_batch`, and `paste_text` send input to a pane's existing
process. `set_synchronize_panes` is the only tool marked as amplifying future
input; the two key-sending tools report every resolved pane target when tmux
copies input across the window.

`call_read_tools_batch` accepts one through 16 serial inspect calls, excluding
itself and the self-bounded `wait_for_text`. Exact tool exclusions prune its
published variants, declared authority, and dispatch together. Selecting only
the aggregate keeps those nested operations callable without advertising them
separately. `onError` selects stop or continue behavior. Completed rows remain
in order, and oversized nested payloads are marked and elided to keep the
complete newline-terminated JSON-RPC response, including its request ID, within
exactly 1,000,000 serialized bytes.

`search_panes` accepts a pattern of at most 256 characters, returns at most
1,024 matches, and uses at most 8,388,608 character-comparison work units.
`wait_for_text` accepts a literal of at most 4,096 characters, uses at most
8 MiB of matching work, and shares one deadline across lookup, connection,
capture, and polling.

Every successful call contains both:

- `structuredContent`, described by the tool's advertised `outputSchema`;
- a JSON-serialized text content item for clients that do not consume
  structured tool results.

The text item is the exact serialized structured value, not a second
hand-formatted rendering. Closed schemas reject unknown fields. Caller text in
tmux format-expanding positions is escaped exactly once; public capability rows
record that control under schema-keyed `inputLiteralization`. String limits
count validated UTF-8 Unicode code points rather than encoded bytes, so runtime
checks match the published schema.

Capability annotations describe requested effects; they are not a sandbox
against trusted-server configuration such as `command-alias`. A dedicated
socket prevents accidental object cross-talk, not hostile local
reconfiguration. See the library's
[Windows safety boundary](../../README.md#windows-through-psmux) for the psmux
registry and process-lifecycle limits.

Every tool uses conservative annotations: `readOnlyHint=false`,
`destructiveHint=true`, `idempotentHint=false`, and `openWorldHint=true`.
The full capability row appears under
`_meta["com.git-pull.libtmux-mcp/capability"]` and matches the row in
`tmux://capabilities`.

## Protocol and lifecycle

The server supports the current `2026-07-28` protocol plus the
`2025-11-25`, `2025-06-18`, `2025-03-26`, and `2024-11-05` initialize
lifecycles on stdio:

- A current client may start with `server/discover`, or call another method
  directly. Every current request carries
  `io.modelcontextprotocol/protocolVersion` and
  `io.modelcontextprotocol/clientCapabilities` in `_meta`; `clientInfo` and a
  `progressToken` are optional. There is no `initialize` or
  `notifications/initialized` exchange.
- A legacy client sends `initialize`, reads the selected version and
  capabilities, then sends `notifications/initialized` before listing or
  calling tools. A known legacy version is echoed. An unknown legacy request is
  negotiated to `2025-11-25`, the newest initialize-based version this server
  supports, and the client may disconnect if it cannot use that selection.

The server advertises tools plus one static resource.
`resources/list` exposes `tmux://capabilities`; `resources/read` returns its
startup-frozen effective rows, connection provenance, and common process
boundary. There are no dynamic resources, templates, subscriptions, or
prompts. Both supported protocol eras expose the same static document.
The advertised tools capability remains `{"tools": {}}`, with no
`listChanged`. `prompts/list` returns `-32601 no such method` rather than an
empty list.

`server/discover` reports `2026-07-28`, the one supported per-request-metadata
version, plus the tools capability, server identity, instructions, and a public
one-hour cache lifetime. Current
`tools/list` results use the same cache policy. Every successful current result
has `resultType: "complete"` and stamps server identity in result `_meta`;
legacy result shapes remain unchanged. An unsupported modern per-request
version returns `-32022` with exact `supported` and `requested` data. A rejected
modern probe does not poison the process, so a stdio client can fall back to a
valid legacy initialize. Once a valid request selects an era, mixing that
conversation with the other lifecycle is rejected explicitly. Legacy `ping`
stays available while initialization or tool work is in progress;
`2026-07-28` removes that method. This server never requests another input
round, so it rejects unrecognized `inputResponses` or `requestState` instead of
replaying a mutating tool as a fresh call.

Tool calls run on four workers, with at most 64 calls in flight. The stdio
reader, lifecycle routing, legacy pings, cancellation, and result writer do not
wait behind a long tool call. Replies may therefore arrive in a different order
than requests; JSON-RPC IDs are the correlation boundary. A request ID remains
reserved until its reply has been written, so a pipelined duplicate cannot be
accepted while the first reply is blocked by client backpressure. Legacy IDs
are never reusable within one process, including the ID used to initialize it.

`wait_for_text` uses tmux control-output events when a control connection can be
opened. It closes the capture/connect race with a capture immediately after
subscribing, checks events in bounded slices, and falls back to 50 ms capture
polling if control mode is unavailable or loses events. One end-to-end deadline
covers target resolution, the initial capture, control-socket expansion,
connection startup, and later captures. The returned `mode` states which path
produced the answer. `pane_id` is present only once the target resolves, so a
wait that expires during lookup reports `mode: pane-lookup` without it.

A `progressToken` in legacy `tools/call` `_meta`, or in a current request's
required `_meta`, enables `notifications/progress` during a wait.
`notifications/cancelled` cooperatively cancels a queued or running call and
suppresses its result when cancellation is observed before the write. As MCP
permits, a result already completing concurrently with cancellation may still
win that race. End-of-input cancels all outstanding work before the process
joins its workers. Cancellation is checked between library operations; an
already-running tmux subprocess remains bounded by libtmux's execution policy
rather than being forcibly killed from another thread.

Input is one JSON-RPC value per line. Exact negotiated `2025-03-26`, after its
initialized notification, also accepts nonempty JSON-RPC batch arrays. That
revision introduced batching; `2024-11-05` predates it, and `2025-06-18` and
later revisions removed it. Those revisions, current MCP, lifecycle batches,
and empty arrays are therefore rejected.

Every batch member is validated independently. Valid notifications occupy no
response slot, an all-notification batch emits nothing, and malformed members
that purport to be requests produce `-32600` entries with a null ID. Responses
are aggregated in input request order after asynchronous calls settle, even if
execution finishes in a different order. Progress notifications remain
standalone messages so a long batch cannot hide progress. All member IDs are
reserved before dispatch, duplicates are rejected before any tool executes,
and reservations remain held until the aggregate response is written.

Once a valid JSON-RPC version and method identify a notification, the server
never replies even when that method's parameters are invalid. Inbound response
messages are also consumed silently, without reserving their IDs, because this
server never originates requests to its peer.

Each input line is bounded at 8 MiB and is drained before an error is returned,
so an oversized request cannot desynchronize the next frame.
Request IDs may occupy at most 512 KiB after JSON serialization. A larger ID
receives a bounded `id: null` invalid-request response before reservation or
tool dispatch, so echoed framing cannot consume the read-batch response budget.

## Build and install

The server supports the same C++23 and C++20 modes as the library. It is off by
default because it is the only component that needs the JSON dependency.

```console
$ cmake \
    -S . \
    -B build/mcp \
    -DLIBTMUX_BUILD_MCP_SERVER=ON \
    -DLIBTMUX_FETCH_DEPS=ON \
    -DLIBTMUX_BUILD_TESTS=OFF \
    -DLIBTMUX_BUILD_EXAMPLES=OFF
```

```console
$ cmake --build build/mcp
```

```console
$ cmake --install build/mcp --prefix ~/.local
```

This installs `libtmux-mcp-server` and its packaged minimal tmux configuration
in the selected binary directory.

On native Windows, build the repository's `windows-psmux` preset and use the
audited psmux version described in [the library README](../../README.md#windows-through-psmux).
The executable advertises the same 45 names. The native smoke records which
operations psmux 3.3.7 supports and requires unsupported commands to fail
explicitly; this remains a bounded preview, not tmux parity.

## Socket selection

The process resolves one socket before protocol startup and never accepts a
per-call override. The shared environment contract is:

| Variable | Meaning |
|---|---|
| `LIBTMUX_SOCKET` | Exact tmux socket name |
| `LIBTMUX_SOCKET_PATH` | Exact socket path; mutually exclusive with the name |
| `LIBTMUX_TMUX_CONFIG` | Explicit absolute tmux configuration path |

With no selector, the server uses the product-dedicated `libtmux-mcp` socket.
With no configuration path, a newly created dedicated socket uses minimal
configuration. Explicitly named, path-pinned, inherited, or already-running
sockets have user-configured or unknown provenance and omit teardown by
default. A wholly new dedicated minimal socket may default to all four
toolsets. `tmux://capabilities` reports the selector, selection provenance,
server state, and configuration provenance. The process stops a dedicated
daemon only when its startup nonce proves that this invocation created it.

CLI selectors remain available:

```console
$ libtmux-mcp-server --socket-path /tmp/tmux-1000/agent
```

```console
$ libtmux-mcp-server --socket-name agent
```

The selectors are:

- `--socket SELECTOR`, where `SELECTOR` is `name:NAME`,
  `path:/ABSOLUTE/PATH`, or `inherit`;
- `--socket-path PATH` for tmux `-S PATH` on POSIX;
- `--socket-name NAME` for tmux or psmux `-L NAME`;
- one positional `PATH`, retained as a POSIX compatibility spelling.

`--tmux-config /ABSOLUTE/PATH` selects an explicit tmux configuration rather
than the packaged minimal file. Earlier no-selector launches followed `$TMUX`;
the current default is the dedicated socket above. Pass `--socket inherit` to
retain that earlier behavior; it requires a valid inherited `$TMUX` route.

Conflicting selectors, empty values, and an empty or relative
`LIBTMUX_TMUX_CONFIG` fail before tmux opens. Windows psmux rejects
socket paths before dispatch. Psmux 3.3.7 ignores
`PSMUX_DATA_DIR` and stores routing state in the Windows profile's `.psmux`
directory; use a high-entropy `--socket-name` and exact session and registry
cleanup rather than assuming the variable isolates it. Psmux also loads user
configuration, so automation should set `PSMUX_CONFIG_FILE` to an explicit
audited file. The native smoke uses a task-owned empty file and restores the
caller's environment.

Inspect the CLI without contacting tmux:

```console
$ libtmux-mcp-server --help
```

```console
$ libtmux-mcp-server --version
```

### Capability selection

`LIBTMUX_TOOLSETS` is an unordered comma-separated subset of `inspect`,
`manage`, `execute`, and `teardown`. All 16 subsets are valid. A wholly empty
value selects no toolsets; once nonempty, leading, trailing, and interior empty
tokens are errors. `LIBTMUX_TOOLS` adds exact names, then
`LIBTMUX_EXCLUDE_TOOLS` removes exact names last. Unknown names fail startup.

`LIBTMUX_SAFETY` is retired. Its presence stops startup with a migration error.

## Client configuration

### POSIX

For an isolated POSIX socket:

```console
$ codex mcp add tmux -- \
    ~/.local/bin/libtmux-mcp-server \
    --socket-path /tmp/tmux-1000/agent
```

```console
$ claude mcp add \
    --transport stdio \
    --scope local \
    tmux -- \
    ~/.local/bin/libtmux-mcp-server \
    --socket-path /tmp/tmux-1000/agent
```

For Claude Desktop, the equivalent entry is:

```json
{
  "mcpServers": {
    "tmux": {
      "command": "/home/you/.local/bin/libtmux-mcp-server",
      "args": ["--socket-path", "/tmp/tmux-1000/agent"]
    }
  }
}
```

### Native Windows with psmux

Windows MCP uses the same 45-name manifest through a trusted psmux route.
Unsupported psmux commands fail explicitly. The audited example below
pre-creates an isolated fixture so its exact selector and configuration are
known before an agent connects. Run it once from native PowerShell outside
psmux; it creates a high-entropy selector, an empty configuration file, and a
state file used by the commands below:

```console
$ & {
    $ErrorActionPreference = "Stop"
    function Remove-ExactArtifact {
        param([string] $Path)
        try {
            Remove-Item -LiteralPath $Path -Force -ErrorAction Stop
        }
        catch [System.Management.Automation.ItemNotFoundException] {
        }
        try {
            $null = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
            throw "artifact still exists after cleanup: $Path"
        }
        catch [System.Management.Automation.ItemNotFoundException] {
        }
    }

    $root = Join-Path $env:LOCALAPPDATA "libtmux-cxx-mcp"
    $statePath = Join-Path $root "psmux-state.json"
    New-Item -ItemType Directory -Force -Path $root -ErrorAction Stop | Out-Null
    $suffix = [Guid]::NewGuid().ToString("N")
    $socketName = "libtmux-cxx-mcp-$suffix"
    $sessionName = "agent-$suffix"
    $configPath = Join-Path $root "$socketName.conf"
    $environmentNames = @(
        "PATHEXT",
        "PSMUX_ACTIVE",
        "PSMUX_CONFIG_FILE",
        "PSMUX_DATA_DIR",
        "PSMUX_NO_WARM",
        "PSMUX_REMOTE_ATTACH",
        "PSMUX_SESSION",
        "PSMUX_SESSION_NAME",
        "PSMUX_TARGET",
        "PSMUX_TARGET_FULL",
        "PSMUX_TARGET_SESSION",
        "TMUX",
        "TMUX_PANE"
    )
    $originalEnvironment = @{}
    foreach ($name in $environmentNames) {
        $originalEnvironment[$name] =
            [Environment]::GetEnvironmentVariable($name, "Process")
    }
    $creationAttempted = $false
    $stateClaimed = $false
    $configClaimed = $false
    $setupFailure = $null
    try {
        foreach ($name in $environmentNames) {
            [Environment]::SetEnvironmentVariable($name, $null, "Process")
        }
        $env:PATHEXT = ".COM;.EXE;.BAT;.CMD"
        $stateJson = [pscustomobject]@{
            socket_name = $socketName
            session_name = $sessionName
            config_path = $configPath
        } | ConvertTo-Json
        $stateBytes = [Text.UTF8Encoding]::new($false).GetBytes($stateJson)
        $stateStream = [IO.File]::Open(
            $statePath,
            [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write,
            [IO.FileShare]::None
        )
        $stateClaimed = $true
        try {
            $stateStream.Write($stateBytes, 0, $stateBytes.Length)
            $stateStream.Flush($true)
        }
        finally {
            $stateStream.Dispose()
        }
        $configStream = [IO.File]::Open(
            $configPath,
            [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write,
            [IO.FileShare]::None
        )
        $configClaimed = $true
        $configStream.Dispose()
        $env:PSMUX_CONFIG_FILE = $configPath
        $env:PSMUX_NO_WARM = "1"
        $creationAttempted = $true
        & tmux.exe -u -L $socketName new-session -d -s $sessionName
        if ($LASTEXITCODE -ne 0) {
            throw "psmux fixture creation failed with exit $LASTEXITCODE"
        }
    }
    catch {
        $setupFailure = $_.Exception.Message
        if (-not $creationAttempted) {
            try {
                if ($configClaimed) {
                    Remove-ExactArtifact -Path $configPath
                }
                if ($stateClaimed) {
                    Remove-ExactArtifact -Path $statePath
                }
            }
            catch {
                $setupFailure += "; prelaunch artifact cleanup failed: " +
                    "$($_.Exception.Message); config=$configPath state=$statePath"
            }
        }
    }
    finally {
        foreach ($name in $environmentNames) {
            try {
                [Environment]::SetEnvironmentVariable(
                    $name,
                    $originalEnvironment[$name],
                    "Process"
                )
            }
            catch {
                $restoreFailure = "failed to restore ${name}: " +
                    "$($_.Exception.Message)"
                if ($null -eq $setupFailure) {
                    $setupFailure = $restoreFailure
                }
                else {
                    $setupFailure += "; $restoreFailure"
                }
            }
        }
    }
    if ($null -ne $setupFailure) {
        if ($creationAttempted) {
            throw "$setupFailure; recovery preserved at $statePath with " +
                "socket=$socketName session=$sessionName config=$configPath"
        }
        throw $setupFailure
    }
    Get-Content -LiteralPath $statePath
  }
```

The recovery state is claimed atomically and the empty configuration is created
before psmux is launched. A concurrent setup cannot overwrite or delete the
first setup's state. If launch is attempted but does not report success, both
artifacts are preserved with the exact identifiers in the error instead of
guessing that no fixture exists; use the guarded cleanup below. Setup clears
inherited tmux and psmux routing variables for the launch, then restores every
original value.

Replace the executable path, then register the native server with Codex. This
uses the documented [Codex stdio environment
configuration](https://developers.openai.com/codex/mcp/):

```console
$ $state = Get-Content "$env:LOCALAPPDATA\libtmux-cxx-mcp\psmux-state.json" | ConvertFrom-Json; codex mcp add tmux-windows --env "PSMUX_CONFIG_FILE=$($state.config_path)" --env PSMUX_NO_WARM=1 -- "C:\path\to\prefix\bin\libtmux-mcp-server.exe" --socket-name $state.socket_name
```

Claude Code uses the same state and explicit selector. `--transport stdio`
also keeps the server name from being parsed as another environment pair, as
required by the [Claude Code MCP
syntax](https://code.claude.com/docs/en/mcp#option-3-add-a-local-stdio-server):

```console
$ $state = Get-Content "$env:LOCALAPPDATA\libtmux-cxx-mcp\psmux-state.json" | ConvertFrom-Json; claude mcp add --env "PSMUX_CONFIG_FILE=$($state.config_path)" --env PSMUX_NO_WARM=1 --transport stdio --scope local tmux-windows -- "C:\path\to\prefix\bin\libtmux-mcp-server.exe" --socket-name $state.socket_name
```

Supplying the same empty `PSMUX_CONFIG_FILE` at creation and MCP launch avoids
loading the user's aliases for this fixture. It does not attest an already
running server's live alias map. Trust the exact psmux binary, the pre-created
server, and its configuration before connecting either client.

Close clients using the MCP server and remove or disable their registration
before cleanup. This paste-and-run command validates the recorded names and
configuration path before using them, kills only the exact session, and stops
without deleting artifacts if the kill or registry audit fails. It never
issues `kill-server`. It isolates routing variables during cleanup and restores
their original values before removing the task artifacts:

```console
$ & {
    $ErrorActionPreference = "Stop"
    function Get-PsmuxRegistryItems {
        param([string] $Root, [string] $Filter)
        try {
            $rootItem = Get-Item -LiteralPath $Root -Force -ErrorAction Stop
        }
        catch [System.Management.Automation.ItemNotFoundException] {
            return
        }
        if (-not $rootItem.PSIsContainer) {
            throw "psmux registry root is not a directory: $Root"
        }
        return @(
            Get-ChildItem -LiteralPath $Root `
                -Filter $Filter `
                -Force -ErrorAction Stop
        )
    }
    function Resolve-PsmuxProfileRoot {
        $candidate = [Environment]::GetEnvironmentVariable(
            "USERPROFILE",
            "Process"
        )
        if (-not [string]::IsNullOrWhiteSpace($candidate)) {
            return $candidate
        }
        $candidate = [Environment]::GetFolderPath("UserProfile")
        if (-not [string]::IsNullOrWhiteSpace($candidate)) {
            return $candidate
        }
        $homeDrive = [Environment]::GetEnvironmentVariable(
            "HOMEDRIVE",
            "Process"
        )
        $homePath = [Environment]::GetEnvironmentVariable(
            "HOMEPATH",
            "Process"
        )
        if (-not [string]::IsNullOrWhiteSpace($homeDrive) -and
            -not [string]::IsNullOrWhiteSpace($homePath)) {
            $candidate = "$homeDrive$homePath"
            if ([IO.Directory]::Exists($candidate)) {
                return $candidate
            }
        }
        $candidate = [Environment]::GetEnvironmentVariable("HOME", "Process")
        if (-not [string]::IsNullOrWhiteSpace($candidate)) {
            return $candidate
        }
        throw "could not resolve the Windows profile used by psmux"
    }
    function Remove-ExactArtifact {
        param([string] $Path)
        try {
            Remove-Item -LiteralPath $Path -Force -ErrorAction Stop
        }
        catch [System.Management.Automation.ItemNotFoundException] {
        }
        try {
            $null = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
            throw "artifact still exists after cleanup: $Path"
        }
        catch [System.Management.Automation.ItemNotFoundException] {
        }
    }

    $root = Join-Path $env:LOCALAPPDATA "libtmux-cxx-mcp"
    $statePath = Join-Path $root "psmux-state.json"
    $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
    $socketName = [string]$state.socket_name
    $sessionName = [string]$state.session_name
    $prefix = "libtmux-cxx-mcp-"
    if ($socketName -cnotmatch '^libtmux-cxx-mcp-[0-9a-f]{32}$') {
        throw "refusing an invalid recorded psmux socket name"
    }
    $suffix = $socketName.Substring($prefix.Length)
    if ($sessionName -cne "agent-$suffix") {
        throw "refusing an invalid recorded psmux session name"
    }
    $expectedConfig = [IO.Path]::GetFullPath((Join-Path $root "$socketName.conf"))
    $recordedConfig = [IO.Path]::GetFullPath([string]$state.config_path)
    $sameConfig = [string]::Equals(
        $expectedConfig,
        $recordedConfig,
        [StringComparison]::OrdinalIgnoreCase
    )
    if (-not $sameConfig) {
        throw "refusing a configuration path outside the task root"
    }
    $config = Get-Item -LiteralPath $expectedConfig -Force -ErrorAction Stop
    $linked = ($config.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
    if ($config.PSIsContainer -or $config.Length -ne 0 -or $linked) {
        throw "refusing a nonempty, directory, or linked task configuration"
    }
    $profileRoot = Resolve-PsmuxProfileRoot
    $registry = Join-Path $profileRoot ".psmux"
    $filter = "$socketName`__$sessionName.*"
    $environmentNames = @(
        "PATHEXT",
        "PSMUX_ACTIVE",
        "PSMUX_CONFIG_FILE",
        "PSMUX_DATA_DIR",
        "PSMUX_NO_WARM",
        "PSMUX_REMOTE_ATTACH",
        "PSMUX_SESSION",
        "PSMUX_SESSION_NAME",
        "PSMUX_TARGET",
        "PSMUX_TARGET_FULL",
        "PSMUX_TARGET_SESSION",
        "TMUX",
        "TMUX_PANE"
    )
    $originalEnvironment = @{}
    foreach ($name in $environmentNames) {
        $originalEnvironment[$name] =
            [Environment]::GetEnvironmentVariable($name, "Process")
    }
    $cleanupFailure = $null
    $fixtureCleanupProven = $false
    try {
        foreach ($name in $environmentNames) {
            [Environment]::SetEnvironmentVariable($name, $null, "Process")
        }
        $env:PATHEXT = ".COM;.EXE;.BAT;.CMD"
        $env:PSMUX_CONFIG_FILE = $expectedConfig
        $env:PSMUX_NO_WARM = "1"
        & tmux.exe -u -L $socketName kill-session -t $sessionName
        if ($LASTEXITCODE -ne 0) {
            throw "exact psmux session cleanup failed with exit $LASTEXITCODE"
        }
        $deadline = [DateTime]::UtcNow.AddSeconds(3)
        do {
            $residue = @(
                Get-PsmuxRegistryItems -Root $registry -Filter $filter
            )
            $live = @(
                $residue | Where-Object {
                    -not $_.PSIsContainer -and
                    $_.Extension -in ".port", ".key", ".pid"
                }
            )
            if ($live.Count -eq 0) { break }
            Start-Sleep -Milliseconds 100
        } while ([DateTime]::UtcNow -lt $deadline)
        if ($live.Count -ne 0) {
            throw "psmux left exact live registry files: $($live.FullName -join ', ')"
        }
        $unknown = @(
            $residue | Where-Object {
                $_.PSIsContainer -or
                $_.Extension -notin ".sid", ".port", ".key", ".pid"
            }
        )
        if ($unknown.Count -ne 0) {
            throw "psmux left unknown exact registry items: $($unknown.FullName -join ', ')"
        }
        foreach ($file in @(
            $residue | Where-Object {
                -not $_.PSIsContainer -and $_.Extension -eq ".sid"
            }
        )) {
            Remove-Item -LiteralPath $file.FullName -Force -ErrorAction Stop
        }
        $sid = @(
            Get-PsmuxRegistryItems `
                -Root $registry `
                -Filter "$socketName`__$sessionName.sid" |
                Where-Object { -not $_.PSIsContainer }
        )
        if ($sid.Count -ne 0) {
            throw "psmux left an exact SID registry file"
        }
        $fixtureCleanupProven = $true
    }
    catch {
        $cleanupFailure = $_.Exception.Message
    }
    finally {
        foreach ($name in $environmentNames) {
            try {
                [Environment]::SetEnvironmentVariable(
                    $name,
                    $originalEnvironment[$name],
                    "Process"
                )
            }
            catch {
                $restoreFailure = "failed to restore ${name}: " +
                    "$($_.Exception.Message)"
                if ($null -eq $cleanupFailure) {
                    $cleanupFailure = $restoreFailure
                }
                else {
                    $cleanupFailure += "; $restoreFailure"
                }
            }
        }
    }
    if (-not $fixtureCleanupProven -or $null -ne $cleanupFailure) {
        if ($null -eq $cleanupFailure) {
            $cleanupFailure = "exact psmux fixture cleanup was not proven"
        }
        throw "$cleanupFailure; recovery state=$statePath socket=$socketName " +
            "session=$sessionName config=$expectedConfig"
    }
    try {
        Remove-ExactArtifact -Path $expectedConfig
        Remove-ExactArtifact -Path $statePath
    }
    catch {
        throw "$($_.Exception.Message); recovery state=$statePath " +
            "socket=$socketName session=$sessionName config=$expectedConfig"
    }
  }
```

On POSIX, to swap installed agent configurations to a checkout and restore them
afterward, use [`tools/mcp/mcp_swap.py`](../../tools/README.md). The script does
not manage the native Windows wrapper or its `--socket-name` state.

## Failure semantics

Malformed JSON-RPC request envelopes or request parameter containers, invalid
lifecycle transitions, unknown tools, and unsupported methods return JSON-RPC
errors.
The `2025-11-25` and current `2026-07-28` revisions report missing, unknown,
wrongly typed, out-of-range, or oversized tool arguments as a completed
`tools/call` result with `isError: true`, so a model can correct the input. The
three older legacy revisions retain their deployed `-32602` behavior. A
malformed `tools/call` envelope, such as a non-object `arguments`, remains
`-32602` in every revision. Current results also carry `resultType: "complete"`;
legacy results do not. Tmux refusing a well-formed call is a tool result with
`isError: true` in every revision. Search does not turn capture failures into
an empty success: the first failed pane capture is surfaced as a tool error.

No exception crosses the protocol boundary. Unexpected C++ failures become a
generic internal JSON-RPC error while the detailed diagnostic stays on stderr.

## Layout

| Path | Purpose |
|---|---|
| [`include/libtmux_consumers/mcp.hpp`](include/libtmux_consumers/mcp.hpp) | Small format-independent tool model and catalog API |
| [`src/tool_model.cpp`](src/tool_model.cpp) | Strict named-argument and UTF-8 code-point validation |
| [`src/tool_catalog.cpp`](src/tool_catalog.cpp) | Capability-aware tool declarations and handlers |
| [`src/wait_for_text.cpp`](src/wait_for_text.cpp) | Shared-deadline control-output and capture-polling wait transport |
| [`src/schema.cpp`](src/schema.cpp) | Tool schemas and era-specific result encoding |
| [`src/protocol_validation.cpp`](src/protocol_validation.cpp) | Request metadata and tool-argument validation |
| [`src/protocol.cpp`](src/protocol.cpp) | Dual-era lifecycle, routing, and call decoding |
| [`src/dispatcher.cpp`](src/dispatcher.cpp) | Bounded workers, cancellation, progress, and serialized replies |
| [`src/batch_response.cpp`](src/batch_response.cpp) | Ordered asynchronous batch aggregation and reservation lifetime |
| [`src/stdio_server.cpp`](src/stdio_server.cpp) | Bounded newline framing and single/batch protocol composition |
| [`src/cli.cpp`](src/cli.cpp) | Fail-closed socket selection |
| [`src/server.cpp`](src/server.cpp) | Executable composition root |
| [`tests/mcp_test.cpp`](tests/mcp_test.cpp) | Direct tool tests against deterministic seams and isolated real tmux |
| [`tests/protocol_test.cpp`](tests/protocol_test.cpp) | Both protocol eras, schemas, backpressure, concurrency, cancellation, framing, and selectors |
| [`tests/windows_psmux_smoke.ps1`](tests/windows_psmux_smoke.ps1) | Native psmux fixture, pinned catalog, explicit unsupported-command behavior, and exact cleanup |

## Related

- [libtmux (C++)](../../README.md) — the library and Windows psmux boundary
- [libtmux-mcp](https://github.com/tmux-python/libtmux-mcp) — the broader
  Python MCP server
- [libtmux (Python)](https://libtmux.git-pull.com) — the original library

## License

MIT. See [LICENSE](../../LICENSE).
