# tmux-workspace

This optional C++ application manages tmux workspaces using CLI11, yaml-cpp
and nlohmann JSON. The implementation is partial. Core libtmux remains free of
these dependencies. The pinned CLI11 fallback builds as a static library to
reduce repeated CLI compilation. An installed CLI11 package keeps its supplied
compiled or header-only form.

Build the application with the pinned development toolchain:

```console
$ cmake --preset cxx-dev \
    -DLIBTMUX_BUILD_WORKSPACE_CLI=ON
```

```console
$ cmake --build --preset cxx-dev \
    --target tmux-workspace \
    --parallel 2
```

Inspect the command reference without starting tmux:

```console
$ build/cxx-dev/apps/workspace/tmux-workspace --help
```

```console
$ build/cxx-dev/apps/workspace/tmux-workspace --command-tree
```

## Inspect a workspace through MCP

Loaded workspaces are ordinary tmux sessions. The separate
[MCP server](../mcp/README.md#build-and-install) can inspect them when both
commands select the same socket. Create a directory for a dedicated socket:

```console
$ socket_dir=$(mktemp -d)
```

Load a workspace without attaching:

```console
$ tmux-workspace load \
    -S "$socket_dir/tmux.sock" \
    -d \
    ./project.yaml
```

Configure your MCP client to launch `libtmux-mcp-server` with `--socket-path`
and the absolute socket path used above, and set `LIBTMUX_TOOLSETS=inspect`.
The server selects one endpoint at startup. When loading with `-L NAME`, use
its `--socket-name NAME` option instead.

Discover tools with `tools/list`, then call `list_sessions`, `list_windows`
with its `session` argument, and `list_panes`. Retain the returned stable IDs.
Use `capture_pane` with `paneId` for visible text, or `wait_for_text` with
`target`, literal `text`, and a bounded `timeout_ms`. Other inspections remain
responsive while a text wait is pending. `tmux://capabilities` reports the
selected endpoint and effective tools. Closing the MCP connection cancels
pending work and leaves this separately loaded tmux session running. See the
[MCP tool reference](../mcp/README.md#core-workflow-quick-reference) for capture
cursors, snapshots and position lookup.

## Shell completion

With `tmux-workspace` on `PATH`, enable completion in the current Bash session:

```console
$ source <(tmux-workspace --generate-completion bash)
```

For Zsh, initialise its completion system before sourcing the script:

```console
$ autoload -Uz compinit && compinit && source <(tmux-workspace --generate-completion zsh)
```

For Fish:

```console
$ tmux-workspace --generate-completion fish | source
```

Completion follows the command parser for nested commands, scoped flags and
enumerated values. The shell completes workspace and output paths, including
filenames containing spaces. Queries do not start tmux or load configuration;
session names and configuration aliases are not suggested. Add the appropriate
command to your shell configuration to enable it in future sessions.

## Current commands

`ls`, `search`, `edit`, `convert`, `import teamocil`, `import tmuxinator`, `debug-info`,
`load` and `freeze` have native services. Ordinary search
uses C++ ECMAScript regular expressions without Python. Whole-word matching
groups alternatives.
Python-only expressions are not supported yet.

`ls --tree` groups workspaces by directory in discovery order. `--full` prints
each parsed configuration as indented JSON; unreadable configurations show
`null`. The flags can be combined. Human names and paths escape terminal
controls while preserving Unicode. JSON and NDJSON retain their data values;
`--tree` changes only human presentation.

Load starts tmux when needed, creates sessions or reuses exact existing names.
A session-name override
applies to the final input. New sessions retain explicit window indexes,
created object identities, command settings, environments and layouts. Failed
builds remove their own session and preserve earlier successful inputs.
Layout names, checksums, tree syntax and pane counts are validated before any
input runs a startup script or changes tmux. Named abbreviations follow the
running daemon's version. A handle opened on an absent socket may use the
selected client's version. Previously bound endpoints, orphan sockets and
other daemon-query failures stop loading. Saved trees accept at most 256
nested levels; tmux handles geometry, resizing and removal of unused cells.
Cold startup verifies the new server and bootstrap session before creating
workspace windows. A startup identity error reports any unverified bootstrap
that may remain. Startup uses a five-second process timeout.
`-2` selects 256-colour mode. Legacy `-8` is rejected before reading a workspace
or reaching tmux because supported tmux versions do not provide 88-colour mode.

Ordinary human load attaches from a foreground terminal, or switches the unique
terminal client viewing the invoking tmux pane. Multiple clients require `-d`.
After all inputs succeed, the final input selects the session, including a reused
session. Load publishes and flushes both output streams before handoff. Handoff
failure leaves loaded changes intact; a later output failure preserves an
existing nonzero load status and reports the completed summary when possible.

Attachment requires a standard descriptor identifying the concrete controlling
tty. If all three standard streams are redirected, use `-d`. JSON/NDJSON requires
`-d` or `--append`. Terminal and caller checks precede mutation. The client is
checked again before switching, but tmux's name-targeted switch leaves a race
after that check. Independent `active-pane` client focus on the invoking
physical window prevents a verified switch; use `-d` or `--append`. Clients on
other physical windows do not prevent switching.

Inside tmux, `--append` adds windows to the current pane's session after
verifying its server identity. `-d` takes precedence if both flags are given.
Append preserves existing windows. Failure retains the borrowed session,
applied settings and new windows; machine output identifies retained window
IDs. An explicit first-window index must name a free slot in that session.

Directories resolve against the configuration and parent directories. Plugins
and custom builders remain unsupported and are rejected.

A document key outside the supported subset is refused by name; a key
prefixed `x-`, at any level, is the exception -- it is accepted, ignored, and
(unlike every other key) left untouched by `convert`. A refusal for any other
key names the `x-` prefix as the way to keep a custom one. `<<: *anchor` and
`<<: [*a, *b]` YAML merge keys resolve at every mapping level; an earlier
merge source and an explicit key both win over a later or merged one, in that
order. A window naming no `layout` is tiled, and its first pane is focused by
default; tmuxp instead stacks halving splits and focuses the last pane. Both
are deliberate differences, not configurable.

Conversion preserves unknown document fields. Human conversion previews by
default; `--yes` saves beside the source, and `--save-to` names a destination.
Import translates supported Teamocil and tmuxinator settings and refuses
unsupported behaviour before saving. Capture records current pane commands,
directories, window names, indexes, focus, layouts and local session/window
options. Indexed options and escaped values survive capture and reload;
`synchronize-panes` is restored after creating panes. Inherited/global options
and environment are omitted. It warns that arguments, history, scripts and
plugins cannot be recovered.

Each command accepts `--json` and `--ndjson` before or after its name. NDJSON
wins when both are present. Load flushes operation events before creating
sessions and ends with one completed or failed result. Machine diagnostics use
stderr. Captured control bytes stay inside escaped JSON strings. Saving uses
an exclusively created temporary file; replacing an existing destination
requires `--force`.
Closed event output keeps completed input results and borrowed-session effects
in the failure summary. A known script failure keeps its status and captured
output if the final script event cannot be delivered.

`load --log-file PATH` appends JSON diagnostic records to a regular file.
`--log-level` defaults to `warning`; `info` includes lifecycle records and
`debug` adds script-output chunks. Other records omit captured script bodies.
Required errors and machine results remain visible at every level. New log
files have owner-only permissions; existing contents and permissions remain.
Invalid destinations fail before backend mutation. A later write failure
disables that file and emits one optional warning after primary output checks.
It preserves child status, cleanup and terminal handoff. A failed write can
leave an incomplete final log record.

## Importing workspaces

Import validates the source and translated workspace before previewing or
saving. Unsupported fields are reported by source path. An existing destination
requires `--force`; a refused import leaves it intact.

```console
$ tmux-workspace import teamocil team.yml --save-to team.json
```

```console
$ tmux-workspace import tmuxinator project.yml --save-to project.json
```

Both formats support session names, roots, windows, pane commands and layouts.

- Teamocil preserves window options and window/pane focus. Pane `commands`
  arrays form one semicolon-joined shell input. The legacy `session` wrapper,
  `splits` and `cmd` spellings are accepted.
- Tmuxinator window command arrays run sequentially in one pane; explicit
  panes may each contain a command array. `pre_window`, window `pre` and
  `synchronize: after` are supported.

Both accept `project_name`, `project_root` and `tabs` aliases. An omitted
session name uses the source filename stem. Null aliases fall back to the
other spelling; conflicting non-null values are refused. Command text
must be strings; tmuxinator command arrays may also contain null entries.
Numeric or boolean values are not converted into executable text.
`pre_window` arrays join with `; `; window `pre` arrays join with ` && ` to
retain short-circuit behaviour. Nonempty window `pre` requires explicit panes.
Teamocil retains the first true focus flag in each window and session.

Relative project roots and Teamocil window roots resolve against the directory
where import runs. Tmuxinator window roots resolve against the project root.
Saved paths are absolute, so moving the imported file does not change them.
Dollar expansion and `~user` path spellings are refused because native workspace
loading uses different expansion rules.

Host lifecycle hooks, named pane titles, startup selectors and
endpoint/attachment settings are unsupported. Tmuxinator imports refuse
unexpanded ERB markup (`<%`) before output or overwrite; imports do not
execute Ruby. Teamocil evaluates no templates, so the same markup in a
Teamocil source is ordinary text and is preserved literally.
Synchronization before pane commands is refused: this builder creates all panes
before sending commands, which changes broadcast recipients. Disabled Teamocil
`synchronize-panes` options and tmuxinator `synchronize: after` are supported.

## Before scripts

`before_script` accepts a command string with quoted arguments. It invokes the
executable directly, so shell operators require an explicit shell command.
Load validates every input's script arguments, working directory and environment
names before creating a session. Scripts run after a new session is created or an append
session is selected, before workspace settings and windows. Reusing an existing
session skips the script. The working directory is the session's configured
`start_directory`, or the invoking directory when that field is absent.

Script stdin is closed. Stdout and stderr retain up to 1 MiB each in
`script_output`; NDJSON also flushes `script-output` records while the child
runs. An output limit or script failure removes only the newly owned session.
Append preserves its borrowed session and reports partial effects. SIGINT and
SIGTERM stop and join the child group and return 130 or 143. When a script exits,
remaining processes in its owned group are terminated before load continues.
Scripts have no fixed process deadline.

## Load progress

Human loads show progress when stderr is a terminal with usable dimensions.
`--progress-format` accepts `default`, `minimal`, `window`, `pane`, `verbose`,
or a template. Counters report delivered pane commands and configured delays;
they do not wait for programs running inside panes to exit.

Templates accept `{session}`, `{window}`, `{workspace_path}`, `{progress}`,
`{summary}`, `{status_icon}`, `{bar}`, `{window_bar}` and `{pane_bar}`. Window
fields include `{window_index}`, `{window_total}`, `{window_progress}`,
`{windows_done}`, `{windows_remaining}` and `{window_progress_rel}`. Pane fields
include `{pane_index}`, `{pane_total}`, `{pane_progress}`, `{pane_done}`,
`{pane_remaining}`, `{pane_progress_rel}`, `{session_pane_total}`,
`{session_panes_done}`, `{session_panes_remaining}`, `{session_pane_progress}`
and `{overall_percent}`. Unknown fields remain literal; `{{` and `}}` print braces.

`--progress-lines` sets the script panel's height: the default is three lines,
zero hides script text, and `-1` uses the available height. The panel retains at
most 256 lines of 4096 bytes, clips wide text to terminal cells and renders
control characters as printable placeholders. Failed loads retain the final
bounded script tail before their error message.

`TMUXP_PROGRESS_FORMAT` and `TMUXP_PROGRESS_LINES` supply defaults; explicit flags
win. `--no-progress`, `TMUXP_PROGRESS=0`, `TERM=dumb`, redirected stderr and
machine output disable the panel. `--color never` and `NO_COLOR` disable its
colours. Resize clears the frame and returns subsequent script text to its
original streams. Progress does not hide the cursor or change terminal modes.

While the panel is active, script output appears there. Stdout redirected to a
file, pipe or another terminal also receives its complete script output once.
Without the panel, both script streams flush to their original destinations as
they arrive. JSON and NDJSON retain their structured capture and event records.

SIGINT and SIGTERM interrupt pane-command delays and are checked between
creation, command, option and focus operations. Failure removes a newly owned
session; append retains its borrowed session and reports created window IDs.

## Editor processes

`edit` uses `VISUAL`, then `EDITOR`, then `vi`. It splits quoted arguments and
passes the resolved workspace path directly without invoking a shell. Inside
double quotes, backslashes escape quotes, backslashes, dollars and backticks;
other backslashes remain literal. Escaped newlines join command lines.

The executable gives an available controlling terminal to the editor and
restores foreground ownership afterward. Machine stdout remains separate from
the editor's terminal. Without a controlling terminal, stdin is closed and
stdout/stderr are captured in the result, up to 1 MiB per stream. Exceeding
either limit stops the child group and returns `OUTPUT_LIMIT`. The command returns the
editor's exit status. SIGINT/SIGTERM cancel the owned child group, including
descendants that retain its pipes. Editing has no fixed process deadline.
Custom input streams supplied to the callable CLI use captured process I/O.

## Optional tmuxp shell

`shell` uses an installed `tmuxp` 1.74.0 console executable. Native workspace
loading does not require Python. Put that executable on `PATH`, or select its
path with `TMUX_WORKSPACE_TMUXP`. The value is one executable path, including
spaces; command strings and interpreter arguments are not accepted. Install
optional shell backends in that executable's Python environment.

`TMUX_WORKSPACE_PYTHON` selects the interpreter tmuxp runs under instead,
matching the environment variable the rest of libtmux's workspace ports use
for the same purpose; it takes precedence over `TMUX_WORKSPACE_TMUXP` when
both are set. Install tmuxp into that interpreter's environment.

Inspect a loaded session with its native Python objects:

```console
$ tmux-workspace shell \
    -S "$socket_dir/tmux.sock" \
    -c 'print(pane.pane_id)' \
    --json
```

To open the standard Python console from a foreground terminal:

```console
$ tmux-workspace shell \
    -S "$socket_dir/tmux.sock" \
    --code
```

Optional session and window arguments select the context. `-S` takes precedence
over `-L`. Opposing startup and vi-mode flags retain their argument order;
the last flag wins. An empty `-c` still executes without entering a console.
Interactive mode requires a controlling terminal identified by a standard
descriptor. Machine modes require `-c`.

With `-c`, human output streams directly; JSON reports `child_status`, `stdout`,
`stderr`, `encoding` and `truncated` at the top level -- matching the other
libtmux workspace ports -- and keeps the same capture nested under
`script_output`. NDJSON flushes `script-output` records with
`stream` and `text`, then a completed or failed result. Captured output includes
the reference runtime's own messages. Each stream is limited to 1 MiB;
overflow stops the child group and reports `output_limit` with bounded capture.
SIGINT or SIGTERM sent to the workspace process cancels its owned child group
and preserves the signal exit status. Interactive exit restores terminal
settings and foreground ownership. Shell execution has no fixed deadline.

## Remaining work

Plugin/custom-builder execution, dynamic session/configuration-name completion, full
configuration/import/capture coverage and supported-platform packaging remain
incomplete. Terminal suspend/resume job
control and non-Linux terminal behavior still need verification. Unsupported
extensions return explicit errors; parser coverage alone does not establish
support for every configuration or execution path.

The application currently requires POSIX tmux. Run tests only with private
sockets. The workspace fixture never reaches the default server.

## Verification

Run the native CLI unit and isolated lifecycle checks, the two verifiers below,
and the cases that prove each verifier still reports a failure:

```console
$ ctest --preset cxx-dev \
    -R '^consumer.workspace' \
    --output-on-failure \
    --no-tests=error
```

The retained verifier checks an installed binary, uses a private socket and
verifies script streaming, cancellation, output limits and closed output.
It records raw repeated timings. Pass an installed tmuxp executable to compare
matching startup, discovery, search and load/capture boundaries.

```console
$ python3 apps/workspace/tools/verify_cli.py \
    --binary /tmp/cxx-workspace-install/bin/tmux-workspace \
    --reference tmuxp \
    --iterations 5 \
    --output /tmp/cxx-workspace-verification.json
```

The process verifier uses a real controlling terminal, redirected machine
stdout, cancellation and an unbounded writer. `--load` adds private tmux servers
for attachment, client selection, retained changes and closed-output checks.

```console
$ python3 apps/workspace/tools/verify_process.py \
    --binary /tmp/cxx-workspace-install/bin/tmux-workspace \
    --load \
    --output /tmp/cxx-workspace-process.json
```
