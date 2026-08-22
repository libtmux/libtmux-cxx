# libtmux-mcp-server

A [Model Context Protocol](https://modelcontextprotocol.io) server for
[tmux](https://github.com/tmux/tmux), built on the C++
[libtmux](../../README.md). It speaks newline-delimited JSON-RPC over stdio and
ships as one native executable.

This is an alpha interface. POSIX tmux has the full twelve-tool catalog below.
Native Windows exposes a four-tool, read-only psmux preview and fails closed for
session and window creation, pane input, capture, search, waits, global pane
discovery, persistent control mode, and socket paths.

## Tool catalogs

These four read-only tools are advertised on POSIX and Windows:

| Tool | Arguments | Result |
|---|---|---|
| `inspect_tmux` | — | Sessions, windows, and panes with their owning IDs |
| `list_sessions` | — | Stable session IDs, names, attachment state, and window counts |
| `list_windows` | `session` | Windows belonging to one exact session |
| `list_session_panes` | `session` | Panes belonging to one exact session |

The session-scoped discovery calls are load-bearing on psmux: window and pane
IDs can repeat between sessions, so clients must retain the returned
`session_id`.

Here, read-only describes the requested tmux operation and its MCP annotation.
Tmux and psmux expand server-side `command-alias` entries before built-in
lookup, so the annotation is not a sandbox against hostile server
configuration. Use only servers and configurations you trust; a private
selector prevents accidental cross-talk, not malicious reconfiguration.
Before parsing any command, psmux 3.3.7 globally removes registry entries it
considers stale and reaps servers it considers orphaned. A socket name cannot
isolate that upstream maintenance; it is not a security boundary from other
local psmux users. See the library's [Windows safety boundary](../../README.md#windows-through-psmux).

POSIX tmux advertises eight additional tools:

| Tool | Arguments | Result |
|---|---|---|
| `create_session` | `name` | The new session ID and name |
| `new_window` | `session`, `name` | The new window ID and its owning session ID |
| `list_panes` | — | Every pane with stable pane, window, and session IDs |
| `capture_pane` | `target` | Visible rendered pane text |
| `send_text` | `target`, `text` | Pane ID after typing literal text |
| `send_keys` | `target`, `keys` | Pane ID after pressing validated key names |
| `wait_for_text` | `target`, `text`, `timeout_ms?` | Match, timeout, elapsed time, transport mode, and final capture |
| `search_panes` | `text` | Matching pane IDs and matching lines |

`send_text` never interprets key names. `send_keys` validates the complete
space-separated key list before sending any key. `timeout_ms` is an integer
from 1 through 60000 and defaults to 10000.

Start with `inspect_tmux`. Tool descriptions tell the model which stable IDs to
retain, and every parameter has a description and a closed JSON Schema.
Misspelt arguments, wrong JSON types, out-of-range integers, and oversized
strings are caller errors; they never reach tmux. String limits count validated
UTF-8 Unicode code points, not encoded bytes, so the runtime and published JSON
Schema apply the same character limit.

Every successful call contains both:

- `structuredContent`, described by the tool's advertised `outputSchema`;
- a JSON-serialized text content item for clients that do not consume
  structured tool results.

The text item is deliberately the exact serialized structured value, not a
second hand-formatted rendering. This follows MCP's structured-content
compatibility guidance and gives old and new clients one authoritative result.

Tools also publish titles and the MCP read-only, destructive, idempotent, and
open-world annotations. `tools/list` is a fixed, unpaginated catalog.

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
produced the answer.

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
tools-only server never originates requests to its peer.

Each input line is bounded at 8 MiB and is drained before an error is returned,
so an oversized request cannot desynchronize the next frame.

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

This installs `libtmux-mcp-server` in the selected binary directory.

On native Windows, build the repository's `windows-psmux` preset and use the
audited psmux version described in [the library README](../../README.md#windows-through-psmux).
The executable advertises only the four psmux-safe read tools; this is a
bounded MCP preview, not tmux parity.

## Socket selection

Choose an exact socket whenever an agent should not reach a person's default
tmux server:

```console
$ libtmux-mcp-server --socket-path /tmp/tmux-1000/agent
```

```console
$ libtmux-mcp-server --socket-name agent
```

The selectors are:

- `--socket-path PATH` for tmux `-S PATH` on POSIX;
- `--socket-name NAME` for tmux or psmux `-L NAME`;
- one positional `PATH`, retained as a POSIX compatibility spelling;
- no selector only inside a valid inherited `TMUX` route.

An absent or invalid inherited `TMUX` value is an error; the MCP server never
silently falls back to the default server. Windows psmux rejects
`--socket-path` and positional paths before dispatch. Psmux 3.3.7 ignores
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

Windows MCP is a connection to a trusted, pre-created psmux session. It exposes
exactly `inspect_tmux`, `list_sessions`, `list_windows`, and
`list_session_panes`; the server cannot create the fixture. Run this setup once
from native PowerShell outside psmux. It creates a high-entropy selector, an
empty configuration file, and a state file used by the commands below:

```console
$ & {
    $root = Join-Path $env:LOCALAPPDATA "libtmux-cxx-mcp"
    $statePath = Join-Path $root "psmux-state.json"
    if (Test-Path -LiteralPath $statePath) {
        throw "clean up the existing libtmux-cxx MCP fixture first"
    }
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    $suffix = [Guid]::NewGuid().ToString("N")
    $socketName = "libtmux-cxx-mcp-$suffix"
    $sessionName = "agent-$suffix"
    $configPath = Join-Path $root "$socketName.conf"
    $oldConfig = $env:PSMUX_CONFIG_FILE
    $oldNoWarm = $env:PSMUX_NO_WARM
    $creationAttempted = $false
    try {
        [IO.File]::WriteAllText($configPath, "")
        [pscustomobject]@{
            socket_name = $socketName
            session_name = $sessionName
            config_path = $configPath
        } | ConvertTo-Json | Set-Content `
            -LiteralPath $statePath `
            -Encoding UTF8 `
            -ErrorAction Stop
        $env:PSMUX_CONFIG_FILE = $configPath
        $env:PSMUX_NO_WARM = "1"
        $creationAttempted = $true
        & tmux.exe -u -L $socketName new-session -d -s $sessionName
        if ($LASTEXITCODE -ne 0) {
            throw "psmux fixture creation failed with exit $LASTEXITCODE"
        }
    }
    catch {
        $message = $_.Exception.Message
        if (-not $creationAttempted) {
            Remove-Item `
                -LiteralPath $configPath, $statePath `
                -Force -ErrorAction SilentlyContinue
            throw $message
        }
        throw "$message; recovery preserved at $statePath with " +
            "socket=$socketName session=$sessionName config=$configPath"
    }
    finally {
        $env:PSMUX_CONFIG_FILE = $oldConfig
        $env:PSMUX_NO_WARM = $oldNoWarm
    }
    Get-Content -LiteralPath $statePath
  }
```

The recovery state and empty configuration are written before psmux is
launched. If launch is attempted but does not report success, both are
preserved with the exact identifiers in the error instead of guessing that no
fixture exists; use the guarded cleanup below.

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
issues `kill-server`:

```console
$ & {
    $root = Join-Path $env:LOCALAPPDATA "libtmux-cxx-mcp"
    $statePath = Join-Path $root "psmux-state.json"
    $state = Get-Content -LiteralPath $statePath | ConvertFrom-Json
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
    $profileRoot = [Environment]::GetFolderPath("UserProfile")
    if ([string]::IsNullOrWhiteSpace($profileRoot)) {
        $homeDrive = [Environment]::GetEnvironmentVariable("HOMEDRIVE", "Process")
        $homePath = [Environment]::GetEnvironmentVariable("HOMEPATH", "Process")
        if (-not [string]::IsNullOrWhiteSpace($homeDrive) -and
            -not [string]::IsNullOrWhiteSpace($homePath)) {
            $profileRoot = "$homeDrive$homePath"
        }
    }
    if ([string]::IsNullOrWhiteSpace($profileRoot)) {
        $profileRoot = $env:HOME
    }
    if ([string]::IsNullOrWhiteSpace($profileRoot)) {
        throw "could not resolve the Windows profile used by psmux"
    }
    $registry = Join-Path $profileRoot ".psmux"
    $filter = "$socketName`__$sessionName.*"
    $oldConfig = $env:PSMUX_CONFIG_FILE
    $oldNoWarm = $env:PSMUX_NO_WARM
    try {
        $env:PSMUX_CONFIG_FILE = $expectedConfig
        $env:PSMUX_NO_WARM = "1"
        & tmux.exe -u -L $socketName kill-session -t $sessionName
        if ($LASTEXITCODE -ne 0) {
            throw "exact psmux session cleanup failed with exit $LASTEXITCODE"
        }
        $deadline = [DateTime]::UtcNow.AddSeconds(3)
        do {
            $residue = @(
                Get-ChildItem -LiteralPath $registry `
                    -Filter $filter `
                    -File -ErrorAction SilentlyContinue
            )
            $live = @(
                $residue | Where-Object {
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
                $_.Extension -notin ".sid", ".port", ".key", ".pid"
            }
        )
        if ($unknown.Count -ne 0) {
            throw "psmux left unknown exact registry files: $($unknown.FullName -join ', ')"
        }
        foreach ($file in @(
            $residue | Where-Object { $_.Extension -eq ".sid" }
        )) {
            Remove-Item -LiteralPath $file.FullName -Force -ErrorAction Stop
        }
        $sid = @(
            Get-ChildItem -LiteralPath $registry `
                -Filter "$socketName`__$sessionName.sid" `
                -File -ErrorAction SilentlyContinue
        )
        if ($sid.Count -ne 0) {
            throw "psmux left an exact SID registry file"
        }
        Remove-Item -LiteralPath $expectedConfig -Force -ErrorAction Stop
        if (Test-Path -LiteralPath $expectedConfig) {
            throw "task configuration still exists after cleanup"
        }
        Remove-Item -LiteralPath $statePath -Force -ErrorAction Stop
        if (Test-Path -LiteralPath $statePath) {
            throw "task state still exists after cleanup"
        }
    }
    finally {
        $env:PSMUX_CONFIG_FILE = $oldConfig
        $env:PSMUX_NO_WARM = $oldNoWarm
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
| [`tests/windows_psmux_smoke.ps1`](tests/windows_psmux_smoke.ps1) | Native psmux fixture, exact four-tool catalog, typed hierarchy, and exact cleanup |

## Related

- [libtmux (C++)](../../README.md) — the library and Windows psmux boundary
- [libtmux-mcp](https://github.com/tmux-python/libtmux-mcp) — the broader
  Python MCP server
- [libtmux (Python)](https://libtmux.git-pull.com) — the original library

## License

MIT. See [LICENSE](../../LICENSE).
