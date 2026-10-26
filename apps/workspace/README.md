<!-- libtmux-logo -->
<div align="center">
  <picture>
    <source srcset="../../assets/workspace.svg" type="image/svg+xml">
    <img src="../../assets/workspace.png" width="128" height="128" alt="libtmux for C++ workspace">
  </picture>
</div>
<!-- /libtmux-logo -->

<div align="center">

# tmux-workspace

[![libtmux.org](https://img.shields.io/badge/docs-libtmux.org-0b7285)](https://libtmux.org/en/cxx/)

Load, capture and convert tmuxp-style workspace files from the command line.

</div>

- An optional, POSIX-only application. The implementation is
  [partial](../../docs/workspace-cli.md#remaining-work).
- Built with CLI11, yaml-cpp and nlohmann JSON; core libtmux stays free of them.
- It builds workspaces through the builder in
  [`examples/workspace/`](../../examples/workspace/README.md), so a program
  calling that builder and this command build a workspace the same way.

Reference: the [command manual][ws-cli], the [guides][ws-guides] and the
[workspace configuration][ws-configuration]. The contract is in
[`docs/workspace-cli.md`](../../docs/workspace-cli.md).

## Quick start

Build and install the command with tests and examples off:

```console
$ cmake -S . -B build/workspace \
    -DLIBTMUX_BUILD_WORKSPACE_CLI=ON \
    -DLIBTMUX_FETCH_DEPS=ON \
    -DLIBTMUX_BUILD_TESTS=OFF \
    -DLIBTMUX_BUILD_EXAMPLES=OFF \
    && cmake --build build/workspace \
    && cmake --install build/workspace --prefix ~/.local
```

That installs `~/.local/bin/tmux-workspace`. Check it without starting tmux:

```console
$ ~/.local/bin/tmux-workspace --version
```

Save this as `example.yaml`: session `example` with an `editor` window split
`main-vertical` between `$EDITOR .` and `pytest -q`, and a `docs` window whose
pane starts in `docs` and runs `mkdocs serve`.

```yaml
session_name: example
windows:
  - window_name: editor
    layout: main-vertical
    panes:
      - $EDITOR .
      - pytest -q
  - window_name: docs
    panes:
      - start_directory: docs
        shell_command: mkdocs serve
```

Load it on a named socket without attaching:

```console
$ tmux-workspace load example.yaml \
    -d \
    -L workspace-example \
    --json
```

This prints a JSON result naming the session it created. Every command accepts
`--json` and `--ndjson` before or after its name.

## Find and load workspaces

[`ls`][ws-ls], [`search`][ws-search] and [`edit`][ws-edit] find workspaces by
name in a project's `.tmuxp.yaml` or in `~/.tmuxp`. Copy the file there first:

```console
$ mkdir -p ~/.tmuxp && cp example.yaml ~/.tmuxp/example.yaml
```

### [`ls`][ws-ls]: list what is saved

[`ls`][ws-ls] lists the workspace files `tmux-workspace` can see, and `--tree`
groups them by directory:

```console
$ tmux-workspace ls
```

```console
$ tmux-workspace ls --tree
```

### [`search`][ws-search]: find workspaces by content

[`search`][ws-search] takes patterns with optional field prefixes. This finds
workspaces that have a window named `editor`:

```console
$ tmux-workspace search window:editor
```

Ordinary search uses C++ ECMAScript regular expressions, without Python.

### [`load`][ws-load]: build a session

[`load`][ws-load] creates or reuses a session from a workspace file, or from a
saved name:

```console
$ tmux-workspace load \
    -d \
    -L workspace-named \
    example
```

A session already running under the name a document asks for is compared with
the document and left as found; one missing a described window stops the load. [Current commands][docs-current] has the
rules for attaching, appending and scripts.

## Capture, convert and import

### [`freeze`][ws-freeze]: capture a running session

[`freeze`][ws-freeze] captures a session back into a workspace file:

```console
$ tmux-workspace freeze example \
    -L workspace-example \
    --save-to frozen.yaml
```

Capture warns that arguments, history, scripts and plugins cannot be recovered.

### [`convert`][ws-convert]: YAML and JSON

[`convert`][ws-convert] converts a workspace between YAML and JSON. A human run
previews by default; `--save-to` names a destination:

```console
$ tmux-workspace convert example.yaml \
    --workspace-format json \
    --save-to example.json
```

### [`import`][ws-import]: Teamocil and tmuxinator

[`import`][ws-import] translates a Teamocil or tmuxinator file, validating it
before it previews or saves. An existing destination needs `--force`:

```console
$ tmux-workspace import teamocil team.yml --save-to team.json
```

```console
$ tmux-workspace import tmuxinator project.yml --save-to project.json
```

[Importing workspaces][docs-import] lists what each format supports.

### [`edit`][ws-edit] and [`debug-info`][ws-debug-info]

[`edit`][ws-edit] opens a saved workspace in `$VISUAL`, then `$EDITOR`, then
`vi`; [`debug-info`][ws-debug-info] prints the runtime, configuration and tmux
diagnostics:

```console
$ tmux-workspace edit example
```

```console
$ tmux-workspace debug-info
```

### [`shell`][ws-shell]: inspect with Python objects

[`shell`][ws-shell] needs an installed `tmuxp` 1.74.0 executable on `PATH`, or
named by `TMUX_WORKSPACE_TMUXP`. It evaluates a snippet against a loaded
session:

```console
$ tmux-workspace shell \
    -L workspace-example \
    -c 'print(pane.pane_id)' \
    --json
```

## Shell completion

[Completion][ws-completion] follows the command parser for nested commands,
scoped flags and enumerated values, and does not start tmux. For Bash:

```console
$ source <(tmux-workspace --generate-completion bash)
```

For Zsh, initialise its completion system first:

```console
$ autoload -Uz compinit && compinit && source <(tmux-workspace --generate-completion zsh)
```

For Fish:

```console
$ tmux-workspace --generate-completion fish | source
```

## Inspect a loaded workspace through MCP

Loaded workspaces are ordinary tmux sessions, so the
[MCP server](../mcp/README.md#quick-start) can inspect them when both
commands select the same socket:

```console
$ tmux-workspace load \
    -L workspace-example \
    -d \
    ./example.yaml
```

- Launch `libtmux-mcp-server` with `--socket-name workspace-example` and
  `LIBTMUX_TOOLSETS=inspect`.
- Call [`list_sessions`][tool-list_sessions], [`list_windows`][tool-list_windows]
  and [`list_panes`][tool-list_panes], and keep the stable IDs they return.
- Read text with [`capture_pane`][tool-capture_pane], or wait for it with
  [`wait_for_text`][tool-wait_for_text].
- Closing the MCP connection leaves the loaded session running. The
  [guide][ws-inspect-guide] walks through it.

## Support and status

- Requires POSIX tmux; tests run only on private sockets, and the workspace
  fixture never reaches the default server.
- Plugins and custom builders are rejected, and some configuration, import and
  capture coverage is incomplete: see
  [what remains](../../docs/workspace-cli.md#remaining-work).
- [Verification](../../docs/workspace-cli.md#verification) runs the CLI and process
  verifiers, and the cases that prove each still reports a failure.

## Develop

Build with the pinned development toolchain, and inspect the command reference
without starting tmux:

```console
$ cmake --preset cxx-dev \
    -DLIBTMUX_BUILD_WORKSPACE_CLI=ON
```

```console
$ cmake --build --preset cxx-dev \
    --target tmux-workspace \
    --parallel 2
```

```console
$ build/cxx-dev/apps/workspace/tmux-workspace --command-tree
```

[ws-cli]: https://libtmux.org/en/cxx/latest/workspace/cli/
[ws-guides]: https://libtmux.org/en/cxx/latest/workspace/guides/
[ws-configuration]: https://libtmux.org/en/cxx/latest/workspace/configuration/
[ws-ls]: https://libtmux.org/en/cxx/latest/workspace/cli/ls/
[ws-search]: https://libtmux.org/en/cxx/latest/workspace/cli/search/
[ws-load]: https://libtmux.org/en/cxx/latest/workspace/cli/load/
[ws-freeze]: https://libtmux.org/en/cxx/latest/workspace/cli/freeze/
[ws-convert]: https://libtmux.org/en/cxx/latest/workspace/cli/convert/
[ws-import]: https://libtmux.org/en/cxx/latest/workspace/cli/import/
[ws-edit]: https://libtmux.org/en/cxx/latest/workspace/cli/edit/
[ws-debug-info]: https://libtmux.org/en/cxx/latest/workspace/cli/debug-info/
[ws-shell]: https://libtmux.org/en/cxx/latest/workspace/cli/shell/
[ws-completion]: https://libtmux.org/en/cxx/latest/workspace/cli/completion/
[ws-inspect-guide]: https://libtmux.org/en/cxx/latest/workspace/guides/inspect-with-mcp/
[docs-current]: ../../docs/workspace-cli.md#current-commands
[docs-import]: ../../docs/workspace-cli.md#importing-workspaces
[tool-list_sessions]: https://libtmux.org/en/cxx/latest/mcp/tools/list_sessions/
[tool-list_windows]: https://libtmux.org/en/cxx/latest/mcp/tools/list_windows/
[tool-list_panes]: https://libtmux.org/en/cxx/latest/mcp/tools/list_panes/
[tool-capture_pane]: https://libtmux.org/en/cxx/latest/mcp/tools/capture_pane/
[tool-wait_for_text]: https://libtmux.org/en/cxx/latest/mcp/tools/wait_for_text/
