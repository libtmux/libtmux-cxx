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

## Current commands

`ls`, `search`, `edit`, `convert`, `import teamocil`, `import tmuxinator`, `debug-info`,
`load -d`, `load --append` and `freeze` have native services. Ordinary search
uses C++ ECMAScript regular expressions without Python. Whole-word matching
groups alternatives.
Python-only expressions are not supported yet.

Load starts tmux when needed, creates sessions or reuses exact existing names.
A session-name override
applies to the final input. New sessions retain explicit window indexes,
created object identities, command settings, environments and layouts. Failed
builds remove their own session and preserve earlier successful inputs.
Cold startup verifies the new server and bootstrap session before creating
workspace windows. A startup identity error reports any unverified bootstrap
that may remain. Startup uses a five-second process timeout.

Inside tmux, `--append` adds windows to the current pane's session after
verifying its server identity. `-d` takes precedence if both flags are given.
Append preserves existing windows. Failure retains the borrowed session,
applied settings and new windows; machine output identifies retained window
IDs. An explicit first-window index must name a free slot in that session.

Directories resolve against the configuration and parent directories. Scripts,
plugins and custom builders remain unsupported and are rejected.

Conversion preserves unknown document fields. Human conversion previews by
default; `--yes` saves beside the source, and `--save-to` names a destination.
Import currently covers names, directories, panes, pre-commands and layouts;
additional importer fields still
need implementation. Capture records current pane commands, directories,
window names, indexes, focus and layouts. It warns that original arguments,
history, scripts and plugins cannot be recovered. This build omits environment
and options from captured documents.

Each command accepts `--json` and `--ndjson` before or after its name. NDJSON
wins when both are present. Load flushes operation events before creating
sessions and ends with one completed or failed result. Machine diagnostics use
stderr. Captured control bytes stay inside escaped JSON strings. Saving uses
an exclusively created temporary file; replacing an existing destination
requires `--force`.

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

## Remaining work

Terminal attachment, Python process services,
progress/logging, shell completion, full configuration/import/capture coverage
and supported-platform packaging remain incomplete. Editor suspend/resume job
control and non-Linux terminal behavior still need verification. The corresponding
process commands and lifecycle flags return explicit unavailable errors. The parser
includes their intended grammar so generated metadata can be reviewed; their
presence in help does not mean those services are finished.

The application currently requires POSIX tmux. Run tests only with private
sockets. The workspace fixture never reaches the default server.

## Verification

Run the native CLI unit and isolated lifecycle checks:

```console
$ ctest --preset cxx-dev \
    -R '^consumer.workspace.cli' \
    --output-on-failure \
    --no-tests=error
```

The retained verifier checks an installed binary, uses a private socket and
records raw repeated timings. Pass an installed tmuxp executable to compare
matching startup, discovery, search and load/capture boundaries.

```console
$ python3 apps/workspace/tools/verify_cli.py \
    --binary /tmp/cxx-workspace-install/bin/tmux-workspace \
    --reference tmuxp \
    --iterations 5 \
    --output /tmp/cxx-workspace-verification.json
```

The editor verifier uses a real controlling terminal, redirected machine
stdout, cancellation and an unbounded writer. It opens no tmux server.

```console
$ python3 apps/workspace/tools/verify_process.py \
    --binary /tmp/cxx-workspace-install/bin/tmux-workspace \
    --output /tmp/cxx-workspace-process.json
```
