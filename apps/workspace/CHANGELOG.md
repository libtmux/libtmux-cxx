# Changelog

Notable changes to `tmux-workspace`.

What changed in each release, for someone deciding whether to take it. The
conventions are in [`.github/WRITING.md`](../../.github/WRITING.md#changelog).
This file covers the workspace command.

The application is released with the library under one version, so every
release has a section here. Changes to the library, and changes that span the
library and the applications, such as documentation, CI and release tooling,
are in the [`libtmux` changelog](../../CHANGELOG.md). `libtmux-mcp-server` keeps
[its own](../mcp/CHANGELOG.md).

The `0.1.0-alpha.1` and `0.1.0-alpha.2` entries were written after those
releases, from the commits each tag carries. Everything from `0.1.0-alpha.3` on
was recorded as it landed.

## Unreleased

## 0.1.0 (2026-11-01)

### Workspace CLI

- `tmux-workspace` is an optional POSIX executable built with
  `-DLIBTMUX_BUILD_WORKSPACE_CLI=ON`; the option fails CMake configuration on
  Windows. It links CLI11 2.7, yaml-cpp 0.8 and nlohmann JSON, which the core
  does not, found with `find_package` or fetched at pinned versions when
  `LIBTMUX_FETCH_DEPS=ON`. (#17)
- `load`, `freeze`, `convert`, `import`, `ls`, `search`, `edit`, `shell`,
  `debug-info` and `--command-tree` cover discovery, loading, capture,
  conversion and Teamocil and tmuxinator imports. Every command takes `--json`
  and `--ndjson`, whose results carry `schema_version`, `command` and
  `status`, and whose streams end with one `completed` or `failed` record. (#17)
- `load`, `freeze` and `shell` select the server with `-S`, then `-L`, then
  `LIBTMUX_SOCKET_PATH`, then `LIBTMUX_SOCKET`, and with none of them pass tmux
  no socket. Before, no flag meant the `TMUX` socket as `-S`, and the variables
  were not read. Pass `-S "${TMUX%%,*}"` for the earlier choice. `--append` and
  an attached `load` still require the selected server to be the pane's.
  `shell` hands tmuxp the `-S` or `-L` that a flag or variable chose, since
  tmuxp reads neither variable. (#128)
- Error codes are lower snake_case and share the vocabulary the other libtmux
  workspace ports use, such as `workspace_not_found`, `invalid_workspace`,
  `session_not_found`, `tmux_unavailable`, `tmux_unsupported`, `tmux_failed` and
  `script_failed`. A tmux older than 3.3a is refused as `tmux_unsupported`
  before `load` goes further, and `shell` refuses before it starts tmuxp.
  (#17, #68)
- `load` starts a cold server and verifies it belongs to the command before it
  creates a workspace window. It resolves YAML merge keys, expands `$VAR`,
  `${VAR}` and a leading `~` in shell commands the way tmuxp does, validates
  layout names, checksums, tree syntax and pane counts before any script runs,
  and sizes a new session to the terminal it runs from. A session that is
  already running is compared with the document, and one missing a window the
  document describes is `session_mismatch`, with nothing built. (#17)
- An interactive `load` asks before moving the current client. An attached
  `load` resolves the invoking pane's context first and reports every way that
  context can be unusable as one usage error with exit 2. A load cancelled by
  a signal keeps what it built and names it under `retained_state`, and a load
  that fails removes the session it created. (#17)
- `freeze` writes block-style YAML, quotes every string scalar a YAML 1.1 or
  1.2 resolver would read back as a boolean, null or number, and refuses a
  session whose name holds `.` or `:`. `import` refuses unexpanded ERB markup
  (`<%`) in tmuxinator files before it writes; Teamocil evaluates no
  templates, so the same markup there is kept literally. (#17)
- `shell` runs an installed tmuxp 1.74.0 executable, found on `PATH` or at
  `TMUX_WORKSPACE_TMUXP`, with the console backends and startup flags tmuxp
  offers; `TMUX_WORKSPACE_PYTHON` names the interpreter it runs under. `edit`
  opens `VISUAL`, then `EDITOR`, then `vi`. (#17)
- `load` shows progress on a terminal with presets, templates and a bounded
  script panel, `--log-file` creates its destination `0600`, and Bash, Zsh and
  Fish completion scripts come from `--generate-completion`. (#17)

## 0.1.0-alpha.14 (2026-10-25)

No entries for `tmux-workspace`.

## 0.1.0-alpha.13 (2026-10-24)

No entries for `tmux-workspace`.

## 0.1.0-alpha.12 (2026-10-18)

### Workspace CLI

- `tmux-workspace` checks the release a selector reaches before `load` goes
  further, including its bootstrap launch, and stops when the daemon is
  refused rather than starting a server on top of it. `load`, `freeze` and
  `shell` report the refusal as `tmux_unsupported`, `shell` before it starts
  tmuxp, and `freeze` no longer reports it as `session_not_found`;
  `tmux_unavailable` stays for a tmux that cannot be reached. (#68)

## 0.1.0-alpha.11 (2026-09-26)

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

## 0.1.0-alpha.10 (2026-09-19)

No entries for `tmux-workspace`.

## 0.1.0-alpha.9 (2026-09-19)

No entries for `tmux-workspace`.

## 0.1.0-alpha.8 (2026-09-12)

No entries for `tmux-workspace`.

## 0.1.0-alpha.7 (2026-09-06)

No entries for `tmux-workspace`.

## 0.1.0-alpha.6 (2026-08-31)

No entries for `tmux-workspace`.

## 0.1.0-alpha.5 (2026-08-22)

No entries for `tmux-workspace`.

## 0.1.0-alpha.4 (2026-08-22)

No entries for `tmux-workspace`.

## 0.1.0-alpha.3 (2026-08-22)

No entries for `tmux-workspace`.

## 0.1.0-alpha.2 (2026-08-17)

No entries for `tmux-workspace`.

## 0.1.0-alpha.1 (2026-08-16)

No entries for `tmux-workspace`.
