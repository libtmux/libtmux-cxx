# Changelog

What changed in each release, for someone deciding whether to take it. The
conventions are in [`.github/WRITING.md`](.github/WRITING.md#changelog).

The `0.1.0-alpha.1` and `0.1.0-alpha.2` entries were written after those
releases, from the commits each tag carries. Everything from `0.1.0-alpha.3` on
was recorded as it landed.

## Unreleased

## 0.1.0-alpha.11 (2026-09-26)

This alpha adds `tmux-workspace`, an optional POSIX executable for workspace
discovery, loading, capture, conversion and Teamocil and tmuxinator imports,
whose JSON and NDJSON output shares the error codes and event names the other
libtmux workspace ports use. Saved layouts are validated before dispatch
through `validate_layout` and `Server::validate_layouts`, and tmux 3.8's v2
JSON layouts are accepted.

A session lookup that misses no longer crashes a tmux 3.2a server, a control
connection reaches a session whose name holds `.` or `:`, and the MCP server
matches session arguments by exact name rather than by prefix.

### Documentation

- The public headers now render as an API reference through Doxygen and
  Breathe, one page per header that declares something. (#12)
- Declaration comments reach that reference. Doxygen reads `///` and treats
  `//` as an ordinary source comment, so every brief description generated
  from `include/` was empty; declarations now carry `///`, which
  [`.github/WRITING.md`](.github/WRITING.md#api-documentation) asks for. (#12)
- The `operator<<` entry in [`docs/api.md`](docs/api.md) no longer loses the
  word `<ostream>`. GitHub read the unbackticked type as an HTML tag and
  dropped it, so a sentence naming the header a caller avoids paying for did
  not name it. (#12)

### Workspace CLI

- Add the optional `tmux-workspace` executable, built with
  `-DLIBTMUX_BUILD_WORKSPACE_CLI=ON`, for workspace discovery, loading,
  capture, conversion and imports, with JSON and NDJSON output. POSIX only;
  the option fails CMake configuration on Windows. (#17)
- The workspace CLI links CLI11 2.7, found with `find_package` or, with
  `LIBTMUX_FETCH_DEPS=ON`, fetched at pinned `v2.7.2`. (#17)
- `load` starts a cold tmux server when none is running, then verifies the
  new server and its bootstrap session belong to it before creating any
  workspace window; a startup identity error names any bootstrap session
  that may remain unverified. (#17)
- `edit` opens `VISUAL`, then `EDITOR`, then `vi` on the workspace path,
  giving it the controlling terminal and reclaiming foreground ownership and
  terminal mode afterward. Without a controlling terminal it captures
  stdout/stderr instead, up to 1 MiB per stream, reporting `output_limit` if
  either overflows; SIGINT/SIGTERM cancel the owned child group, including
  descendants holding its pipes. (#17)
- Validate layout names, checksums, tree syntax and pane counts before startup
  scripts or tmux changes. Version-sensitive abbreviations follow the running
  daemon; tmux retains responsibility for sizing and pruning saved layouts. (#17)
- `load -2` forces 256-colour tmux mode on the client it starts; `-8` is
  refused as `usage` before reading the workspace, since no supported tmux
  offers 88-colour mode. (#17)
- Execute optional shells through an installed tmuxp 1.74.0 executable, with
  literal arguments, streamed output, bounded failures and terminal restoration.
  Select its path with `TMUX_WORKSPACE_TMUXP`; `shell` honours
  `TMUX_WORKSPACE_PYTHON` for the interpreter tmuxp runs under, taking
  precedence over `TMUX_WORKSPACE_TMUXP` when both are set. (#17)
- `shell` selects tmuxp's console backend with `--best`, `--pdb`, `--code`,
  `--ptipython`, `--ptpython`, `--ipython` or `--bpython`, and configures its
  startup with `--use-pythonrc`/`--no-startup` and
  `--use-vi-mode`/`--no-vi-mode`; opposing flags keep their argument order,
  and the last one wins. (#17)
- Add terminal load progress with presets, templates and a bounded script panel.
  Stdout redirected to a file, pipe or another terminal receives script output
  once; machine streams retain structured events. `--no-progress` streams both
  script destinations directly. (#17)
- Interrupt pane-command delays and check cancellation between workspace changes.
  Failed appends report retained window IDs; owned builds roll back their
  session. A build failure that coincides with an interruption reports the
  interruption rather than the tmux command failure it raced. A load
  cancelled by a signal keeps what it had built, naming it under
  `retained_state`; a load that fails removes the session it created. (#17)
- Add Bash, Zsh and Fish completion for commands, flags, enumerated values and
  file paths. Generate scripts with `--generate-completion`. (#17)
- Preserve native Teamocil and tmuxinator command grouping, directories, focus
  and options during import. Refuse unsupported behaviour before saving. (#17)
- Refuse unexpanded ERB markup (`<%`) in tmuxinator imports before output or
  overwrite. Teamocil evaluates no templates, so the same markup there is
  preserved literally. (#17)

- `freeze` writes block-style YAML; window options go under `options_after`,
  applied before the panes exist, and a pane's `shell_command` is omitted
  when it runs the session's default shell. `freeze` and `convert` quote
  every string scalar a YAML 1.1 (PyYAML/tmuxp) or 1.2 resolver would read
  back as a boolean, null or a number. (#17)
- `freeze` refuses a session whose name carries `.` or `:`, which tmux reads
  as target separators and which `load` could not read back, and reports
  `no session named <name>` for a socket whose server is not running,
  whether or not a name was given. It treats any ordinary shell (`bash`,
  `zsh`, `dash`, `ash`, `ksh`, `mksh`, `fish`, `csh`, `tcsh` or `sh`) as a
  plain pane's default command whenever it matches the session's
  `default-shell` only in which ordinary shell it names -- on macOS
  `/bin/sh` is bash. (#17)
- `load` resolves YAML merge keys (`<<: *anchor` or `<<: [*a, *b]`) at every
  mapping level, with earlier merge sources and explicit keys winning over
  later or merged ones, matching PyYAML. It also expands `$VAR`, `${VAR}`
  and a leading `~` in `shell_command`, `shell_command_before` and
  `before_script` from the loading process's environment before sending
  them to tmux, matching tmuxp; a plain-string pane (`panes: [echo $VAR]`)
  is covered too, and unset variables stay literal. (#17)

- Machine error codes are lower snake_case and share the vocabulary the other
  six libtmux workspace ports converged on: `workspace_not_found`,
  `invalid_workspace`, `unsupported_key`, `session_not_found`,
  `tmux_unavailable`, `tmux_failed`, `script_failed` and `destination_exists`;
  every other code is lower snake_case too. Every stderr error record also
  carries `"schema_version":1`. (#17)
- A condition another libtmux workspace port also reports takes that port's
  shared code: a search pattern that fails to compile is `usage`; a tmux
  command that fails capturing a session, or a server whose identity
  changes during startup, is `tmux_failed`; a workspace file that cannot be
  read or encoded is `invalid_workspace`; and a child command that cannot
  be started -- including a before_script that is missing or not
  executable -- is `script_failed`. A saved layout the running tmux is too
  old for is `tmux_failed`, not `invalid_workspace`; a layout name no tmux
  accepts is still `invalid_workspace`, and a value
  `workspace_builder_options` could not act on is `invalid_workspace`,
  while an unrecognised key beside it is only a warning. (#17)

- `load --json`'s envelope carries only `schema_version`, `command`,
  `status`, `results` and `errors` at the top level; each result record
  carries `reused` (`true` for an appended or already-running session,
  `false` for one this command created) alongside `action`, `input`,
  `input_index`, `session_id` and `session_name`, and `results[]` carries a
  record for every attempted input, failed ones included. (#17)
- `load --ndjson` emits `session-created` directly after `workspace-started`,
  before any window, and a typed `window-created`, `window-completed`,
  `pane-created` and `pane-completed` event for each stage, carrying
  `input_index`, `session_id`, `window_id` and, for panes, `pane_id` and
  `pane_index` -- the vocabulary the other libtmux workspace ports agree on.
  Its `script-completed` event carries `child_status` and `truncated`
  directly, alongside the `script_output` object, matching go. (#17)
- `shell -c --json` reports `child_status`, `stdout`, `stderr`, `encoding`
  and `truncated` at the top level, matching dotnet, go, java, rs and ts,
  alongside the `script_output` object and `exit_code`. (#17)

- A human `load` attaches from a foreground terminal, or switches the one
  client viewing the invoking pane; that client is checked again immediately
  before the switch, since tmux's own name-targeted switch can still race
  after the first check. Independent `active-pane` focus on the invoking
  physical window means tmux's client-pane projection cannot confirm which
  pane that client is looking at, so the switch is refused rather than
  risked; clients on other physical windows do not block it. (#17)
- An interactive `load` asks before moving the current client: inside tmux,
  whether to switch, load detached or append to the current session; for a
  session that already exists, whether to attach. Asked only when stdin is a
  terminal and `--yes` was not given, and scoped to the one input it is
  about -- with two inputs where the second already exists and prompts,
  declining leaves only that one unbuilt, with `action: "left"` in its
  result record. (#17)
- An attached `load` resolves the invoking pane's context before it builds
  anything, and reports every way that context can be unusable as one usage
  error, exit 2: a `TMUX` that does not parse (naming the variable and its
  `socket,pid,session` shape), a server that has since restarted, a
  `TMUX_PANE` that is not a pane id or names no pane, a pane with no
  terminal, and a pane no client views. A `run-shell` key binding, where
  `TMUX` is set but `TMUX_PANE` is not, switches the client without needing
  `-d`; onto a server other than the invoking pane's, `load` exits 2 and
  names `-d`. (#17)
- `load` onto a session that is already running compares it against the
  document: a session missing a window the document describes is
  `session_mismatch`, `status: error`, exit 1, naming the first missing
  window and carrying every one as `missing_windows`, and nothing is built.
  Windows the document leaves unnamed are not compared. (#17)
- `load --append`'s human summary reads `Appended <session>`, naming the
  session that received the windows. Declining the "already running.
  Attach?" prompt gets a plain statement of what was left unchanged,
  alongside `status: ok` and exit 0. A refusal that a command needs a
  foreground terminal is `usage`, exit 2, like every other refusal about
  how the command was invoked. (#17)

- Add `ls`, listing discovered workspaces with `name`, `path`, `format`,
  `size`, `mtime` and `session_name`; `--full` adds each workspace's parsed
  content, `null` when it fails to parse, and `--tree` groups the human
  listing by directory. (#17)
- Add `search`, matching one or more ECMAScript patterns against `name`,
  `session_name`, `path`, window names and pane commands, or `-f`/`--field`
  (repeatable) to search only named fields; a `field:` prefix on a pattern
  scopes that pattern alone. `-i` and `-S` (unless a pattern holds an
  uppercase character) ignore case, `-F` matches literal text, `-w` matches
  whole words, `--any` accepts any pattern instead of requiring every
  pattern, and `-v` inverts the result. (#17)
- Add `--command-tree`, printing the parsed command and option tree as JSON
  without starting or querying tmux. (#17)
- `ls` and `search` colour their human output when `--color` is `always`, or
  under the `auto` default when stdout is a real terminal; `--color never`,
  `NO_COLOR` or machine mode disable it, and `FORCE_COLOR` forces it outside
  a terminal. (#17)
- `--version` prints `tmux-workspace <version>`, and `debug-info` prints a
  readable rendering in human mode (`--json` keeps the object) without the
  constant `home` field. `ls`, `search` and `debug-info` carry the same
  machine envelope as every other command -- `schema_version`, `command`
  and `status` -- and their `--ndjson` streams end with one `completed`
  record; `search`'s `--json` results sit under `results`. Machine output
  (`--json`, outside a saved file) is one compact line. (#17)

- A `--save-to` destination that cannot be written because of the path it
  names -- no such directory, not a directory, no permission -- is a usage
  error naming the destination, exit 2. A saved destination (`freeze -o`,
  `convert --save-to`, `import --save-to`) gets the permissions a plain
  file create would give it under the caller's umask. (#17)

- `--log-file` creates its destination `0600`, refusing a non-regular target,
  including a symlink, as `log_file_unavailable`. `--log-level` selects
  `debug`, `info`, `warning` (the default), `error` or `critical`; a write
  failure afterward disables the file and reports one `log_file_write_failed`
  warning without changing the operation's exit status. (#17)
- `--json` and `--ndjson` are read from the parsed command line, so a value
  like `load -s --json w.yaml` creates a session named `--json` and prints
  human output; the same holds for every other value-taking option. (#17)
- `load` warns when a `start_directory` is not a directory, naming the
  path; tmux still starts that pane in `$HOME`. It also sizes a new session
  to the terminal it runs from
  (`TMUXP_DETECT_TERMINAL_SIZE`/`TMUXP_DEFAULT_COLUMNS`/`TMUXP_DEFAULT_ROWS`,
  matching tmuxp), applied attached and detached, inside and outside tmux; a
  malformed `COLUMNS`/`LINES`/`TMUXP_DEFAULT_*` is a usage error. (#17)

### Layouts

- Add `validate_layout` for pure syntax checks and `Server::validate_layouts`
  for indexed batch checks against the selected daemon. `Window::select_layout`
  and MCP validate saved trees before dispatch; MCP rejects malformed layouts
  before target lookup. (#17)
- Accept v2 JSON saved layouts on tmux 3.8, including `3.8-rc`, while
  preserving floating panes. Older daemons refuse JSON layouts before mutation.
  (#17)
- `Server::validate_layouts` preserves a daemon-version query failure as a
  failure; only a native subprocess handle opened on an absent socket falls
  back to its own client version instead. (#17)
- `parse_version` now accepts a release-candidate suffix, `-rc` or a numbered
  `-rc2`, as the release it names with no revision, instead of
  `VersionError::malformed`, and treats any version ending `-master`, not only
  the bare `master`, as unbounded. (#17)

### Server

- `Server::control_with_options` resolves a session name holding `.` or `:`
  to its id before attaching; tmux's own `-t =name` target splits on both, so
  such a session was previously unreachable through a control connection.
  (#17)
- On Windows, `Server::session` strips a trailing `:` the same way it already
  strips a leading `=`, so a `=name:` target matches the named session there
  too; previously the trailing `:` made it match nothing. (#17)
- Any lookup that resolves a `Session`, such as `Server::session`, no longer
  crashes the tmux server on tmux 3.2a when the target does not exist. The
  `session_created` field now expands inside a lazy session-identity
  conditional instead of dereferencing a session tmux has none of. (#17)

### Entities

- `Client` rows carry `pid`, `active_pane_id`, `flags` and `window_id`, so a
  caller can read a client's process id, physical window and focus flags
  from the same snapshot as its terminal and session. `active_pane_id()` and
  `window_id()` return `PaneId` and `WindowId`; use `.value()` for text. (#17)

### MCP server

- `kill_session`, `rename_session`, `list_windows`, `get_session_info`,
  `set_history_limit` and `create_window` resolve their `session` argument to
  an exact match by listing sessions and comparing names, rather than handing
  tmux a bare or `=`-prefixed target that can resolve by prefix; a stale or
  mistyped name now reports no such session instead of acting on a
  differently named one. (#17)

### Examples

- `libtmux::workspace::build_windows()` splits the pane a previous step just
  created, not the window it lives in, so a window with three or more panes
  and no explicit layout builds its panes in the order the document gives
  them. (#17)
- `libtmux::workspace::build_windows()` waits for a pane's shell to draw its
  prompt before sending that pane's command, so the terminal does not echo
  the command before the prompt and show it twice; it retries the
  cursor-position query, up to a bounded deadline, when the query itself
  fails. (#17)
- `libtmux::workspace::build_windows()` escapes a window's name and start
  directory before handing them to tmux, matching the escaping it already
  applies to every pane it splits: tmux expands `#{...}` in both. (#17)
- `libtmux::workspace::build_windows()` applies a window option tmux keeps in
  its window table (such as `pane-base-index`) to every window it builds,
  rather than only to whichever window happens to be current when the
  session's `options:` are set; a window's own `options:` still have the
  last word on that window. (#17)
- With no pane declaring `focus`, `libtmux::workspace::build_windows()` leaves
  the last pane it built in a window active, matching tmuxp. An explicit
  `focus` still wins, and which window a session is on is unchanged. (#17)
- A `libtmux::workspace::BuildError` from a failed split names only the tmux
  diagnosis; it no longer carries the library's internal escaping format
  string for the command that failed. (#17)
- `libtmux::workspace::parse_tmuxp()` treats a document, window, pane or
  command key prefixed `x-`, at any level, as inert -- accepted and ignored --
  instead of refusing the whole document; every other unsupported-key refusal
  names the prefix as the way to keep a custom key. (#17)
- `libtmux::workspace::parse_tmuxp()` reads `workspace_builder_options`
  instead of refusing the whole document as an unsupported key; a setting
  this builder does not have is reported in `Workspace::warnings` rather than
  refusing the document. (#17)
- `libtmux::workspace::build()`/`build_windows()` take an optional
  width/height, passed to `new_session` as tmux's own `-x`/`-y`, so a session
  built with an explicit size stays that size until a client attaches; every
  window built before then is laid out at that size rather than tmux's
  `default-size` (80x24 unless changed). (#17)
- `BuildEvent`, the argument a `BuildObserver` callback receives, carries
  `session_id`, `window_id` and `pane_id`, populated once each build stage's
  session, window or pane exists. A callback given to `BeforeBuild` or
  `BuildObserver` may return a `BuildStop{reason, retain}`; a retained stop
  keeps what the build has made rather than rolling it back. (#17)

## 0.1.0-alpha.10 (2026-09-19)

This alpha publishes what `0.1.0-alpha.9` tagged. That release's Windows port
build failed before any artifact reached the registry, so this is the first
build carrying the typed entity surface and completion waits described below.

### vcpkg

- The port declares static linkage on Windows, so a dynamic triplet —
  `x64-windows` or `x64-windows-release` — installs the static library the
  preview can produce instead of failing to configure.

## 0.1.0-alpha.9 (2026-09-19)

This alpha types the entity surface and replaces polling with completion
events. Sessions, windows and panes carry distinct id types, so a pane id no
longer passes where a window id belongs; `CommandObserver` receives a
`CommandReport` carrying the arguments and elapsed time behind a command; and
`ExecutionPolicy::tmux_binary` names the executable for command and control
paths alike. Each of those is source- and ABI-breaking, and `Pane::kFields` is
wire-breaking for handwritten positional recordings.

Waiting no longer re-reads snapshots. `CommandRuntime` blocks or hands out a
descriptor an event loop can select on, `CommandOperation` waits without being
consumed, and `Pane::wait_for_text` waits for output tmux has confirmed rather
than for the caller's own echo. `Server::over` takes a transport the
application owns and `Server::over_control` reuses held-open control clients
instead of one process per command.

The library installs as static or shared on POSIX, with version macros and a
pkg-config file for consumers that do not use CMake.

### Breaking

- Entity IDs now distinguish sessions, windows and panes. Use
  `pane.id().value()` where a string is needed. Source- and ABI-breaking. (#18)
- `CommandObserver` now receives a `CommandReport`, including arguments and
  elapsed time. Read `report.command` and `report.failure` in existing
  observers. Source- and ABI-breaking. (#18)
- `Pane::kFields` gains position and exit-status fields. Update handwritten
  rows to follow it. Wire-breaking for positional recordings. (#18)
- `ExecutionPolicy::tmux_binary` selects the executable for commands and
  control clients. `ConnectionOptions::tmux_binary` becomes optional; read it
  with `value_or("tmux")`. Source- and ABI-breaking. (#18)

### Entities

- Numeric fields now reject malformed values instead of reading them as
  zero. (#18)
- Reading an older recording with fewer fields no longer reads beyond its
  row. (#18)
- Linked tmux windows and panes now compare equal across sessions. (#18)

### Queries

- Add `first_owned` and `exactly_one_owned` to retain a selected value after
  its source range is gone. (#18)
- Filters now cover every exposed entity field, including command and buffer
  listings. (#18)

### Errors

- Add `as_command_failure` and `as_protocol_error` to convert between command
  and control-mode failures. (#18)

### Server

- Socket-path constructors now accept filesystem paths directly. (#18)
- Add `Server::over` to use a caller-supplied command transport. (#18)
- Add `Server::over_control` to reuse control connections for supported
  commands. (#18)
- Add environment methods to read, set, forget or remove variables inherited
  by new processes. (#18)
- Startable servers now honour the requested socket selector when starting
  the daemon. (#18)
- Wait channels and option or hook names beginning with `-` now reach tmux
  literally, preventing unintended option parsing. (#18)

### Windows

- `Window::select_layout` now rejects unsupported layouts before dispatch,
  preventing daemon crashes on tmux 3.3 and 3.3a. (#18)

### Pane

- Add `Pane::exit_status` to read the status of an exited pane retained by
  `remain-on-exit`. (#18)
- Add `Pane::left` and `Pane::top` to read pane coordinates. (#18)
- Add `Pane::toggle_zoom` to toggle a pane's zoom state. (#18)
- Add `Pane::wait_for_text` and `Server::wait_for_text` to await confirmed
  output; `output_confirms` checks text already captured. (#18)

### Asynchronous commands

- `CommandRuntime` adds blocking waits and a readable descriptor for completed
  observations, replacing caller-side polling. (#18)
- `CommandOperation` adds timed waits that preserve the operation, with
  cancellation when stop tokens are available. (#18)

### Control mode

- `Connection::set_pane_output` now restores output after a pane was muted.
  (#18)
- Control-mode layout data now agrees with ordinary snapshots on tmux 3.8+.
  (#18)
- Subscription-change notifications now expose their session, window and
  pane IDs. (#18)

### MCP server

- `wait_for_text` discounts echoed input from `send_keys`, `paste_text` and
  `run_shell_command`, including submitted commands and wrapped lines. (#18)
- Timed-out or cancelled `run_shell_command` calls now release the pane once
  the command finishes. (#18)
- Tool refusals now return tool error results instead of protocol errors.
  (#18)

### Build

- Version macros let consumers check the library version when compiling.
  (#18)
- The installed pkg-config file supports consumers that do not use CMake.
  (#18)
- `BUILD_SHARED_LIBS` now selects static or shared libraries on POSIX;
  Windows rejects shared builds at configure time. (#18)

## 0.1.0-alpha.8 (2026-09-12)

This alpha reaches the standard library from the typed surface. An entity and a
`CommandFailure` format with `std::format` through the renderer `operator<<`
already used, a field handle is a ranges projection and — for a flag — a
predicate, so the names that build filters also drive the standard algorithms,
and `Pane::send_line` submits a line in one tmux invocation.

`libtmux::bad_expected_access` names what `value()` throws in either standard's
build. The README's C++ is quoted from an example that compiles, and it now
shows what the library returns rather than how to convert it.

### Entities

- `Session`, `Window`, `Pane` and `Client` format with `std::format`, through
  the renderer `operator<<` already used. `libtmux::to_string(entity)` returns
  that text, and a format spec works: `std::format("{:>24}", pane)`.
- Add `Pane::send_line`, which sends literal text and then Enter as one tmux
  invocation. Empty text sends Enter alone; `send_text` still refuses it.

  Before: `pane.send_text("make"); pane.send_key("Enter");`
  After:  `pane.send_line("make");`

### Queries

- A field handle is now a ranges projection, and a flag handle a predicate, so
  the names that build filters also drive the standard algorithms:
  `std::ranges::sort(windows, {}, libtmux::window::index)` and
  `std::ranges::count_if(panes, libtmux::pane::active)`.

### Errors

- Add `libtmux::bad_expected_access<Error>`, the exception `value()` throws,
  aliased to the underlying type so a caller can catch it in the C++20 build
  as well as the C++23 one.
- `CommandFailure` formats with `std::format`, naming what happened, what tmux
  said, the exit code when there is one, and how far the command got:
  `tmux refused the command: can't find session: nope (exit 1, replied)`.
  `libtmux::to_string(failure)` returns the same line.

## 0.1.0-alpha.7 (2026-09-06)

This alpha is wire-breaking for MCP clients and source-compatible for the
library. One immutable capability registry now governs every advertised tool,
frozen at startup and reported through `tmux://capabilities`, and pane input is
authenticated against live topology immediately before it is dispatched. The
configuration switcher is a native `mcp-swap` recording a checksummed
transaction before any write, and socket paths are taken from configuration
rather than from tmux, which escapes them on 3.4 and 3.5.

### Breaking

- `enter_copy_mode` and `exit_copy_mode` are gone from the MCP tool surface.
  Wire-breaking for MCP clients; the core `Pane` operations are unchanged.

  Before: `{"name": "enter_copy_mode", "arguments": {"target": "%1"}}`
  After:  `{"name": "capture_pane", "arguments": {"paneId": "%1"}}`

- A tool that acts on one pane names that argument `paneId` rather than
  `target`, and takes a canonical `%`-prefixed id. Wire-breaking for MCP
  clients. `wait_for_text`, `show_option` and `show_hooks` keep `target`, which
  is any tmux target expression rather than one pane.

  Before: `{"name": "capture_pane", "arguments": {"target": "%1"}}`
  After:  `{"name": "capture_pane", "arguments": {"paneId": "%1"}}`

- `LIBTMUX_SAFETY` stops MCP startup with a migration error rather than being
  honoured. Behavioural.

  Before: `LIBTMUX_SAFETY=read-only libtmux-mcp-server`
  After:  `LIBTMUX_TOOLSETS=inspect libtmux-mcp-server`

### MCP server

- Replace the legacy tool catalog with one immutable 45-tool capability
  registry that governs registration, calls, schemas, descriptions, internal
  input-sink claims, annotations, nested authority, and disclosure.
- Remove `enter_copy_mode` and `exit_copy_mode` from the MCP surface. Use
  `capture_pane`, `capture_since`, `snapshot_pane`, or `search_panes` for
  terminal text; the core `Pane` operations remain available.
- Add startup-frozen `LIBTMUX_TOOLSETS`, `LIBTMUX_TOOLS`, and
  `LIBTMUX_EXCLUDE_TOOLS` selection. `LIBTMUX_SAFETY` now stops startup with a
  migration error.
- Add one process-wide socket selected by `LIBTMUX_SOCKET` or
  `LIBTMUX_SOCKET_PATH`. A new product-dedicated socket uses the bundled
  minimal configuration unless `LIBTMUX_TMUX_CONFIG` names an absolute path;
  only its authenticated creator enables teardown by default and stops it when
  stdio closes.
- Add the static `tmux://capabilities` resource and matching
  `com.git-pull.libtmux-mcp/capability` metadata to every advertised tool.
  Public rows expose schema-keyed input literalization while sink claims remain
  internal validation data.
- Add bounded typed read batching that retains every executed row within a
  1,000,000-byte newline-terminated JSON-RPC response, deterministic bounded
  pane search, and synchronized-pane target disclosure. Request IDs over 512
  KiB fail before dispatch rather than consuming that response budget.
  `set_synchronize_panes` declares that it amplifies subsequent pane input.
- Pane-input tools reject malformed state snapshots and require their
  configured targets to be live, input-enabled, outside human-owned modes and
  terminal attention, and distinct from the caller immediately before
  dispatch. `send_keys` and each `send_keys_batch` row preflight the effective
  synchronized cohort, while `paste_text` uses one private target-only buffer
  for text and optional Enter. Process-wide pane leases prevent overlapping
  input and remain held for uncertain shell runs. `run_shell_command` also
  requires one configured target running a supported POSIX foreground shell
  and uses collision-free, subshell-isolated completion framing through the
  exact tmux endpoint while preserving bounded Bash and Zsh error/debug traps.
### MCP switcher

- Replace `tools/mcp/mcp_swap.py` with a native `mcp-swap`, built beside the
  tests. It preserves the formatting of each agent config it edits, locks
  across concurrent runs, and records one checksummed transaction before any
  write so a failure rolls back in reverse order.
- `get_server_info` reports the socket path the handle was configured with, and
  `wait_for_text` opens its control connection on that path. tmux escapes a
  non-printable byte in the path it stores at server start, so asking it for
  `#{socket_path}` returned a path naming no file on tmux 3.4 and 3.5 — reported
  to the caller as the resolved socket, and enough to make streaming fall back
  to polling. 3.3a and earlier answer with the raw byte, and 3.6 does again.

- `mcp-swap use-local` validates every selected agent config and backup
  destination before changing any config. A malformed later config leaves the
  whole selection unchanged.

## 0.1.0-alpha.6 (2026-08-31)

This is a source- and ABI-breaking alpha. Asynchronous commands now run under
a caller-owned bounded runtime with explicit admission, cancellation,
observation, and shutdown, while POSIX execution preserves causal failures and
one tmux server incarnation. Independent notification watches, bounded control
framing, descriptor-isolated child launches, and the experimental Windows
preview follow the same fail-closed ownership rules.

### Breaking

- `LIBTMUX_ABI_NAMESPACE` advances to `v2_cxx20` and `v2_cxx23`.
  Binary- and ABI-breaking; rebuild every object that links libtmux. (#10)

- `CommandFailure::delivery` replaces `dispatched` with `not_started`,
  `written`, `replied`, and `indeterminate`. Source- and ABI-breaking; treat
  only `not_started` as safe to retry without more evidence. (#10)

- `FailureKind` gains `overloaded` and `cancelled`. Source-breaking for an
  exhaustive switch; add both cases or a `default`. (#10)

- Typed `Server` calls no longer run over control mode. `over_control`,
  `over_control_with_options`, the `Server` notification methods,
  `BackendKind::control_mode`, and
  `ServerFeature::receives_asynchronous_notifications` are removed; use a
  subprocess-backed `Server` for final command status and `Connection` for
  raw control blocks and events. (#10)

- `Connection::execute` now returns every guarded block in
  `ControlRequestResult::blocks`. The exact-count overload, `Attribution`, and
  per-operation results are removed; use `Server::run` when final command
  status matters. (#10)

- `Session::attach_command` now returns
  `expected<AttachCommand, CommandFailure>`, and `checked_attach_command` is
  removed. Source-breaking; retain the `AttachCommand` until the spawned
  client exits so its pinned socket route remains valid. (#10)

- `NotificationKind` gains `config_error`, `exit`, `layout_change`, and
  `message`. Source-breaking for an exhaustive switch; add the cases or a
  `default`. (#10)

- `Server::try_submit(CommandRuntime&, ...)` replaces
  `Server::submit(...)`. Source-breaking; replace
  `server.submit(command)` with `server.try_submit(runtime, command)`, where
  `runtime` is the successful value from `CommandRuntime::start()`. (#11)

### Server

- Add `Server::submit`, which returns a move-only `CommandOperation` to cancel
  or collect later. Several commands can run concurrently, each with its own
  timeout and output limit. (#10)

- Dropping an unconsumed `CommandOperation` detaches observation without
  waiting for or cancelling an accepted command. (#10)

- A submitted command whose deadline expires before it starts, including a
  zero-timeout command, fails with `not_started` without dispatch or side
  effects. (#10)

- Process exit no longer waits indefinitely for a detached custom backend call
  that has no timeout. (#10)

- `CommandObserver` and `CommandFailure::diagnostic` redact typed environment,
  option, hook, and shell values. Raw `CommandRequest` callers can mark the
  same data with `CommandArgument::sensitive` or `sensitive_range`. (#10)

- POSIX entity handles pin the socket incarnation they came from, so a retained
  handle cannot compare equal to or mutate an object on a replacement server.
  (#10)

- POSIX command output drains fairly across concurrent children, so an earlier
  noisy child cannot starve a later one. Output larger than the platform pipe
  buffer is collected up to the configured limit. (#10)

- `Server`, `Connection`, and `ScopedTmuxServer` no longer pass a caller's
  blocked or ignored signals into tmux or pane commands. (#10)

- Add `CommandRuntime`, a move-only owner for bounded asynchronous admission,
  transport threads, observer disposition, counters, and deterministic
  shutdown. `CommandRuntimeShutdown::safe_to_unload` is true only after
  transports, result and observer obligations, and active callback teardown
  end; prevent later runtime use before unloading. (#11)

- `Server::try_submit` returns refusals before admission. After it returns a
  `CommandOperation`, tmux, transport, cancellation, and runtime publication
  failures are that operation's result; custom backends are refused before
  running. (#11)

- Accepted asynchronous commands enter transport in admission order. One
  runtime slot remains occupied until its transport, result, and observer
  obligations all end; `snapshot` exposes current obligations and cumulative
  admission and completion counts. (#11)

- `CommandOperation::detach` and destruction release only the result
  obligation. Waiting, detaching, or dropping an operation never dispatches or
  discards its accepted global observation. (#11)

- Asynchronous `CommandObserver` callbacks run only on the
  `CommandRuntime::dispatch_ready` caller. `discard_ready` releases them
  without invocation, and neither path runs under an internal lock or on a
  transport thread. (#11)

- POSIX asynchronous commands now propagate reactor wake, poll, drain, signal,
  and child-status failures instead of treating them as success. Synchronous
  cleanup uses the same delivery classification, and later cleanup does not
  replace the first failure. (#11)

- POSIX asynchronous commands stop draining output after a bounded post-exit
  grace when a descendant retains stdout or stderr, so an exited child cannot
  remain pending on inherited pipes indefinitely. (#11)

- POSIX ordinary and control-mode children no longer inherit unrelated file
  descriptors. On Linux builds without native close-from support, this assumes
  no concurrent privileged increase of the hard descriptor limit. (#11)

### Control mode

- Add `Connection::watch_notifications`, returning independent
  `NotificationWatch` cursors with their own wait, readiness descriptor, and
  dropped-event count. One consumer no longer drains another. (#10)

- Concurrent `Connection` calls keep independent deadlines, and notification
  reads continue while another caller waits to write. (#10)

- `Connection` recognizes every notification defined by supported tmux
  releases and decodes extended output without treating later fields as pane
  data. (#10)

- Control-event retention is bounded by bytes and record count, and an
  oversized or unterminated line fails without growing memory indefinitely.
  Per-reader drop counts still report lost events. (#10)

### Pane

- `Pane::break_out` keeps a multi-pane break in its source session and returns
  the existing window when the pane is already alone. It previously let tmux
  choose another session implicitly. (#10)

### Windows

- On psmux, `Server::submit` returns before the command completes. Cancelling
  a command proven to have started reports `cancelled` with `indeterminate`
  delivery rather than pretending it never ran. (#10)

- On psmux, `Server::try_submit` returns after admission and before the command
  completes. Cancelling a command proven to have started reports `cancelled`
  with `indeterminate` delivery rather than pretending it never ran. (#11)

- A psmux session or window name ending in `.<digits>` is rejected while other
  dotted names remain valid, so a target-shaped name cannot be misread as
  `session:window.pane`. (#10)

## 0.1.0-alpha.5 (2026-08-22)

The library is unchanged from `0.1.0-alpha.4`: nothing outside comments moved
in `include/`, `src/` or `apps/`. This release carries what the notification
surface says about itself.

### Control mode

- Both `take_notifications` overloads state that taking drains: an empty result
  means nothing has arrived yet rather than that nothing will, and a later batch
  is new traffic rather than a repeat. A caller reacting to events is pointed at
  `wait_for_notifications` instead of polling. (#8)
- `Connection::dropped_notifications` carries documentation, which it did not
  before; it reached the generated reference as a bare declaration. (#8)
- [`docs/design/pane-output-streaming.md`](docs/design/pane-output-streaming.md)
  records what each way of taking the stream costs, so the choice between them
  is measured rather than guessed. Owning an event loop through
  `notification_fd` costs the same wakeups as blocking in
  `wait_for_notifications`; a sleep-and-take loop costs about a hundred times
  as many. (#8)

### Examples

- `06-streaming` demonstrates `notification_fd` under `poll`, so the path a
  program with its own event loop takes is compiled and run rather than only
  described. (#8)

## 0.1.0-alpha.4 (2026-08-22)

`0.1.0-alpha.3` was tagged but did not publish, so this is the release to take
for everything listed under it as well.

### MCP server

- A request that finished before stdin closed is answered rather than
  discarded. Shutdown cancelled every request still in flight, and the reply a
  worker had already produced went with it, so a client that wrote its
  requests and closed the pipe lost answers for whatever was still running.
  (#6)

### Testing

- `ScopedTmuxServer` no longer leaves an unreaped child when a start fails
  past its deadline. The reap waited on the deadline whose expiry brought it
  there, so it returned at once and handed a live process to the reaper
  thread. (#6)

## 0.1.0-alpha.3 (2026-08-22)

### Breaking

- `FailureKind` gains `unsupported`, reported when a backend will not provide
  an operation rather than approximating it. Source-breaking for an exhaustive
  switch built with `-Werror=switch`; add the case, or a `default`. (#4)

  ```cpp
  case libtmux::FailureKind::unsupported:
    std::printf("this backend cannot provide the operation safely\n");
    break;
  ```

- A raw command that synchronously inserts control reply blocks is now rejected
  on a control connection unless the call declares its exact reply count.
  Behaviourally breaking for a caller relying on the inferred count, which does
  not inspect live aliases. Use the counting overload, a low-level `Connection`,
  or a subprocess-backed `Server`. (#4)

### Windows

- Add an experimental x64 desktop preview, built with Visual Studio 2022 against
  [psmux](README.md#windows-through-psmux) 3.3.7 at a pinned commit and archive
  hash. It answers exact read-only entity queries and declines the rest with
  `unsupported` rather than approximating it: typed option and hook access, pane
  input and capture, and control mode are all refused. Ask
  `Server::capabilities()` before choosing a workflow. (#4)
- The MCP server advertises four read-only tools there — `inspect_tmux`,
  `list_sessions`, `list_windows` and `list_session_panes` — and builds on MSVC
  now that the protocol version is compared as text. (#4)

### Library

- Add `Server::capabilities()`, reporting the implementation behind a server,
  the backend in use, and which features it will serve. Asking is how a caller
  learns what a route supports; the alternative is discovering it by failure.
  (#4)
- Add `Snapshot::take_in_session`, which takes a listing scoped to one session
  and optionally under a timeout. (#4)
- Add `same_entity_id`, which compares entity identity. On Windows a psmux
  session keeps its own server, so an id is only unique beside its session id;
  on POSIX the id alone still decides. (#4)
- `SocketError` gains `path_unsupported`, reported when a socket path is asked
  of psmux, which addresses servers by name only. `kSocketPathLimit` states the
  platform's limit, and is zero where paths do not apply. (#4)
- `Version` parsing stops at the first line of `tmux -V` output and refuses a
  number that would overflow, rather than wrapping it. (#4)

### Control mode

- Inserted control replies are attributed without stealing concurrent output.
  `source_file` is declined, because a file can add an unknowable number of
  reply blocks; `check_file` remains. (#4)
- `Pane::break_out` works around raw tmux 3.7, which crashes on an unnamed
  multi-pane `break-pane` and ignores an explicit name. It selects the safe form
  inside one server command and repairs a requested name by stable window ID.
  tmux 3.7a fixes both upstream. (#4)
- Add `control_with_options` and `over_control_with_options`, so streaming
  policy enters through either `Server` doorway without rebuilding the socket
  route. (#4)

### MCP server

- The tool catalog grows from five tools to twelve. `inspect_tmux`,
  `list_windows`, `list_session_panes`, `create_session`, `send_keys`,
  `wait_for_text` and `search_panes` join the original five, which are
  unchanged. (#4)
- Tools publish an `outputSchema` and answer with `structuredContent`, so a
  client validates a reply instead of parsing prose out of it. (#4)
- Accept JSON-RPC batches under the protocol versions that allow them, and
  answer in input order. A member that is a notification, or not a request at
  all, contributes no reply. (#4)
- Negotiate five protocol versions: `2026-07-28`, and the legacy
  `2025-11-25`, `2025-06-18`, `2025-03-26` and `2024-11-05`. (#4)
- Add `server/discover`, which answers the modern discovery request with the
  catalog and its cache policy. (#4)
- Run calls on a worker pool with a bounded number in flight. Past the bound
  the server answers `-32003` rather than queueing without limit. (#4)
- Emit `notifications/progress` for a call carrying a `progressToken`, and
  honour `notifications/cancelled` for one already running. (#4)
- Take `--socket-name` or `--socket-path`. Without a selector only a valid
  inherited `TMUX` route is accepted, so the server never silently drives a
  tmux the caller did not name. (#4)
- A wait whose deadline expired during target lookup no longer publishes an
  empty `pane_id`, which its own output schema refuses. The field is emitted
  only once a target resolves, and is no longer required. (#4)

### vcpkg

- Release updates are monotonic, locked, recoverable and opt-in, and the exact
  tagged port is gated across Linux, macOS, Windows and WSL. Windows support in
  the port is an opt-in transition a release's tagged gates must clear; the
  immutable `0.1.0-alpha.2` entry stays POSIX-only whatever a later one does.
  (#4)

## 0.1.0-alpha.2 (2026-08-17)

The library is unchanged from `0.1.0-alpha.1`. This release is packaging: the
repository became a vcpkg registry, and two faults a user would meet before
reaching the library were fixed.

### vcpkg

- This repository now serves a vcpkg git registry, so installing needs neither
  an overlay port nor a clone of this tree. The project is too new for the
  curated registry.
- Add the versions database and `python3 -m tools.vcpkg check`, which fails when
  the git-tree recorded for a port's declared version is not that port's
  git-tree at `HEAD`. `x-add-version` does not report that case loudly enough to
  gate on, and the registry then serves the old port while the repository shows
  the new one.
- Add the `mcp` feature to the port.
- Fix the port's usage text, which vcpkg had been generating heuristically and
  getting wrong: it named `libtmux::testing` beside the library, but the package
  config defines that target only under `COMPONENTS testing`, so the snippet a
  consumer was handed failed to configure.
- Fix the README's install sequence, which did not run. Naming a registry makes
  the default registry's baseline mandatory, and vcpkg enforces that while
  loading the configuration — before `x-update-baseline --add-initial-baseline`
  could supply it. The manifest and the initial baseline now come first.
- Pushing a tag now rewrites the port and publishes it from the registry in the
  same run. The port fetches its archive by hash, and that hash cannot exist
  before the tag does, so the step necessarily follows tagging and had been
  getting forgotten.
- `probe` takes `--repository`, so the gate can resolve the port over the public
  URL instead of only through a local path. A local path resolves a commit that
  was never pushed, which is the failure a first consumer meets.
- Keep the port's debug tree out of `share`.
- The release notes now name the archive hash the port actually needs, rather
  than the `git archive` hash, which differs.

### MCP server

- `--prefix` now searches `tools/libtmux/` as well as `bin/`. The vcpkg port
  installs the server there, because a static triplet has no `bin/` for it, so
  pointing an agent CLI at a vcpkg install failed with `is not an executable`
  naming a path the package never wrote.

## 0.1.0-alpha.1 (2026-08-16)

First release: the library, the MCP server, and the test fixture, against tmux
3.2a through `master`.

- `Server`, `Session`, `Window` and `Pane` as value types that copy, compare,
  hash and print. An entity is one row of a shared snapshot and reads its fields
  without reaching tmux; the process ran once, when the snapshot was taken.
- Failure is a value. Every call answers `expected<T, CommandFailure>`, and
  nothing throws to report a tmux or transport failure.
- Typed queries over tmux's own fields: `FilterExpr`, composed with `&&`, `||`
  and `!`, working with standard ranges. A filter that asks a number whether it
  starts with a string does not compile, and `tests/compile/` holds the programs
  proving it stays that way.
- Control mode as a second transport behind the same calls, holding one
  connection open.
- Two standards. C++23 over `std::expected`, or C++20 over pinned `tl::expected`
  with `LIBTMUX_CXX_STANDARD=20`, each in its own inline namespace so objects
  built against one cannot link against the other.
- `libtmux::testing`, installed beside the library behind
  `find_package(libtmux COMPONENTS testing)`, so a consumer's suite gets the
  same private-socket tmux fixture this one uses.
- An MCP server, so an agent can drive tmux directly.
- No dependencies in the core.
