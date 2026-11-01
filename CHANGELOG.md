# Changelog

Notable changes to `libtmux`.

What changed in each release, for someone deciding whether to take it. The
conventions are in [`.github/WRITING.md`](.github/WRITING.md#changelog). This
file covers the library.

The applications keep their own changelogs:
[`libtmux-mcp-server`](apps/mcp/CHANGELOG.md) and
[`tmux-workspace`](apps/workspace/CHANGELOG.md). The library and the
applications are released together under one version, so every release has a
section in each file. Changes that span them, such as documentation, CI and
release tooling, are listed here.

The `0.1.0-alpha.1` and `0.1.0-alpha.2` entries were written after those
releases, from the commits each tag carries. Everything from `0.1.0-alpha.3` on
was recorded as it landed.

## Unreleased

## 0.1.0 (2026-11-01)

This is the first release without an `-alpha` tag. It ships the C++ library, the
optional `tmux-workspace` command and `libtmux-mcp-server`, against tmux 3.3a
through `master`, in C++23 or C++20. Linux, including WSL, and macOS are
supported, with an experimental x64 desktop Windows preview through psmux. This
section describes what 0.1.0 ships, and the alpha sections below record each
alpha. Since `0.1.0-alpha.14` the library, `tmux-workspace` and the MCP server
pass tmux no socket unless `LIBTMUX_SOCKET_PATH` or `LIBTMUX_SOCKET` names one,
so a program reaches the server a bare `tmux` command reaches. Breaking lists
what that changes.

### Breaking

- `Server::at_default()` and `Server::startable_at_default()` read
  `LIBTMUX_SOCKET_PATH` and `LIBTMUX_SOCKET`, and reach the server `TMUX` names
  inside tmux. Behaviour-breaking; source-compatible. Before, they ignored the
  variables and pinned the default socket whatever `TMUX` said. (#128)

  Before: `Server::at_default()`
  After: `Server::at_socket_name("default")` for the default socket whatever the
  variables and `TMUX` say

### Server

- `Server` reaches a tmux server by socket path (`at_socket_path`, `-S`), by
  socket name (`at_socket_name`, `-L`), by the choice tmux makes (`at_default`)
  or by the server the process runs inside (`from_env`). The `startable_at_*`
  constructors build handles that may create an absent server, and
  `Server::over` builds one over a transport the caller supplies.
- Add `socket_from_environment()`, `EnvironmentSocket` and `SocketSource` in
  `libtmux/socket.hpp`, which report which variable chose the socket. The path
  wins when both variables are set, a variable that is empty or only whitespace
  counts as unset, and a value tmux cannot use fails the default handle naming
  the variable and not its value. (#128)
- `Server::socket_path()`, the pinned route and a control client from a handle
  that names no socket describe the server a bare `tmux` reaches, which inside
  tmux is the one `TMUX` names. Before, control mode attached to the default
  socket. (#128)
- `Server::capabilities()` reports the implementation behind a handle, its
  backend and which `ServerFeature` it serves, and runs no command. (#4)
- POSIX handles pin the socket incarnation they came from, so a retained
  handle neither compares equal to nor mutates an object on a replacement
  server. (#10)
- `environment()`, `set_environment()`, `unset_environment()` and
  `remove_environment()` read, bind, forget and withhold the variables new
  processes inherit. (#18)
- A wait channel, an option name or a hook name that begins with `-` reaches
  tmux literally and is never read as a flag. (#18)
- `Server::run` runs any one tmux command. `run_batch` sends several in one
  invocation as a fail-fast group that is partially applied and not rolled
  back, and `run_chain` refuses a target it cannot address before tmux runs.

### Command execution

- `ExecutionPolicy` carries a timeout, an output limit and `tmux_binary`, which
  names the executable for command and control paths alike. A bare name
  resolves through `PATH`, and a path with a separator is used as given. (#18)
- POSIX commands drain output fairly across concurrent children, collect output
  larger than the pipe buffer up to the configured limit, and stop draining
  after a bounded grace period when a descendant keeps stdout or stderr open
  past the child's exit. (#10, #11)
- A child inherits neither unrelated file descriptors nor the caller's blocked
  or ignored signals. (#10, #11)
- `CommandObserver` receives a `CommandReport` with the command as tmux received
  it, its argv, the failure if any and the elapsed time. Typed environment,
  option, hook and shell values are redacted from reports and from
  `CommandFailure::diagnostic`; a raw `CommandRequest` marks the same data with
  `CommandArgument::sensitive` or `sensitive_range`. (#10, #18)
- A wake, poll, drain, signal or child-status failure is the command's failure
  and is never reported as success. Later cleanup does not replace the first
  failure. (#11)

### Entities

- `Session`, `Window`, `Pane` and `Client` copy, compare, hash, print and format
  with `std::format`. `std::format("{}", window)` writes `Window(@1 2:editor)`,
  a format spec works, as in `std::format("{:>24}", pane)`, and
  `libtmux::to_string(entity)` returns the same text.
- An entity is one row of the listing that produced it. Reading a field costs
  nothing and never fails, and answers for the moment it was listed;
  `refresh()` returns a new value and leaves the old one as it was.
  `Pane::window`, `Pane::session`, `Session::windows` and `Window::panes` each
  read again.
- `SessionId`, `WindowId` and `PaneId` are distinct types, so a pane id does
  not pass where a window id belongs. `.value()` returns the text. (#18)
- Windows and panes linked into several sessions compare equal across them, and
  `same_entity_id` compares identity where psmux scopes an id by its session.
  (#4, #18)
- `Client` rows carry `pid`, `active_pane_id`, `flags` and `window_id`, so one
  snapshot gives a client's process, physical window and focus flags. (#17)
- `Session::attach_command()` returns an `AttachCommand`, which carries the
  argv and the private route selecting this server incarnation. Keep it alive
  until the spawned client exits. (#10)

### Pane

- `Pane::send_text` and `send_key` send literal text and one key. `send_line`
  sends literal text and then Enter as one tmux invocation; empty text sends
  Enter alone, which `send_text` refuses.
- `Pane::capture` reads what a pane shows, and `CaptureOptions` selects a line
  range or the whole history, joins wrapped lines, and keeps escape sequences
  or trailing spaces.
- Add `Pane::exit_status` for a pane retained by `remain-on-exit`,
  `Pane::left` and `Pane::top` for its coordinates, and `Pane::toggle_zoom`.
  (#18)
- `Pane::break_out` keeps a multi-pane break in its source session and returns
  the existing window when the pane is already alone. On tmux 3.7 it selects
  the form that does not crash the server and repairs a requested name by
  stable window ID. (#4, #10)

### Waiting

- `Pane::wait_for_text` and `Server::wait_for_text` wait for output tmux has
  confirmed, not for the caller's own echo, and answer with a `WaitResult`
  naming the `WaitPath` that produced the answer. `output_confirms` checks text
  already captured. (#18)
- On tmux 3.7 and newer the wait streams control-mode output. On an older
  daemon it polls and never attaches a control client, and a control stream that
  drops while the pane lives keeps waiting by polling. (#69)
- A wait whose pane or server goes away returns `pane_died` with tmux's words in
  `reason` and the last capture in `text`, on every path. A pane that exits
  successfully without capture output during shutdown keeps its last capture;
  a blank pane still replaces the previous output. (#69, #94)

### Queries

- `FilterExpr` is built from typed fields and composes with `&&`, `||` and `!`.
  A filter that asks a number whether it starts with a string does not compile,
  and `tests/compile/` holds the programs that prove it. Boolean fields compose
  directly with `&&` and `||`, and string and boolean fields accept `!=`.
  (#110)
- Filters cover every exposed entity field, including command and buffer
  listings, and `children_of` and `parent_of` join two listings on an id so a
  filter can ask about related rows. (#18)
- A field handle is a ranges projection, and a flag handle is a predicate:
  `std::ranges::sort(windows, {}, libtmux::window::index)` and
  `std::ranges::count_if(panes, libtmux::pane::active)`.
- `matching`, `first` and `exactly_one` select from a listing already in
  memory without reaching tmux, reporting `CardinalityError::none_matched` or
  `several_matched` otherwise. They require forward ranges that yield lvalue
  references and reject const temporaries, since the result borrows. Keep a
  named owner alive, or use `first_owned` and `exactly_one_owned` to keep a
  selected value after its source is gone. (#18, #106)

### Errors

- Failure is a value. Every call answers `expected<T, CommandFailure>`, and
  nothing throws to report a tmux or transport failure. `FailureKind` is
  `validation`, `spawn`, `pre_exec`, `pipe`, `timeout`, `refused`, `missing`,
  `truncated`, `unsupported`, `overloaded` or `cancelled`; `unsupported` means
  the backend will not provide the operation rather than approximate it.
- `CommandFailure::delivery` is a `DeliveryStatus`: `not_started`, `written`,
  `replied` or `indeterminate`. Only `not_started` is safe to retry without
  more evidence. (#10)
- `CommandFailure` formats with `std::format`, naming what happened, what tmux
  said, the exit code when there is one and how far the command got:
  `tmux refused the command: can't find session: nope (exit 1, replied)`.
  `libtmux::to_string(failure)` returns the same line.
- `libtmux::bad_expected_access<Error>` is what `value()` throws, aliased to the
  underlying type so a caller catches it in the C++20 build as well as the
  C++23 one.
- `as_command_failure` and `as_protocol_error` convert between command and
  control-mode failures. (#18)

### Asynchronous commands

- `CommandRuntime::start` takes a `CommandRuntimeConfig` with a fixed admission
  capacity, and owns the transport threads. `Server::try_submit(runtime,
  command)` returns a move-only `CommandOperation`, so several commands run
  concurrently, each with its own timeout and output limit. (#10, #11)
- A refusal before admission, such as a full, stopped or moved-from runtime or
  an invalid command, is the outer `unexpected`. After admission every failure
  is the operation's result, and a command whose deadline expires before it
  starts fails with `not_started` and no side effect. (#11)
- `CommandOperation::wait` collects the result, `wait_for` and `wait_until`
  time out without consuming the operation, `request_cancel` asks the transport
  to withdraw the command, and `detach` releases only the result obligation.
  Dropping an operation detaches observation without waiting for or cancelling
  an accepted command. (#10, #11, #18)
- Accepted commands enter transport in admission order. One runtime slot stays
  occupied until its transport, result and observer obligations all end, and
  `snapshot` reports current obligations with cumulative admission and
  completion counts. (#11)
- Asynchronous `CommandObserver` callbacks run only on the caller of
  `CommandRuntime::dispatch_ready`, never under an internal lock or on a
  transport thread. `discard_ready` releases them without invoking them. (#11)
- `CommandRuntime::wait_ready` blocks for completed observations and
  `ready_fd` is a descriptor an event loop can select on. `close` blocks until
  the threads stop, and `safe_to_unload` is true only once every obligation has
  ended. (#11, #18)

### Control mode

- `Server::control` and `control_with_options` open a `Connection` to one
  session, and `Server::over_control` returns a `Server` whose supported
  commands travel over held-open control clients. `Connection::execute` returns
  every guarded block in `ControlRequestResult::blocks`. (#10, #18)
- `Connection::events` waits for what tmux reports. `take_notifications` is one
  draining cursor, `watch_notifications` returns independent `NotificationWatch`
  cursors with their own wait, readiness descriptor and dropped-event count,
  and `notification_fd` lets a program that owns an event loop select on the
  stream. (#8, #10)
- `Connection` recognises every notification the supported tmux releases
  define and decodes extended output without treating later fields as pane
  data. Subscription-change notifications carry their session, window and pane
  IDs, and `set_pane_output` restores output after a pane was muted. (#10, #18)
- Retention is bounded by bytes and record count. An oversized or unterminated
  line fails without growing memory, and `Parser` retains the exact reply prefix
  when its byte limit cuts a line. Concurrent calls keep independent deadlines,
  and notification reads continue while another caller waits to write. (#10,
  #107)
- `parse` returns views into a notification and rejects a const temporary, so
  keep the notification alive while using the parsed views. (#105)
- Library-managed control mode needs tmux 3.7. On an older daemon
  `Server::control`, `control_with_options`, `over_control` and
  `Connection::connect` return a `ProtocolError` naming 3.7 before any client
  attaches. (#69)

### Layouts

- `validate_layout` checks a saved layout's syntax, and
  `Server::validate_layouts` checks a batch against the selected daemon.
  `Window::select_layout` validates before dispatch, as does the MCP
  `select_layout` tool, which rejects malformed layouts before target lookup.
  (#17)
- JSON saved layouts, `{"V":2,...`, need tmux 3.8 and keep floating panes. An
  older daemon is refused with `FailureKind::validation`, saying "JSON saved
  layouts require tmux 3.8 or newer". The mirrored `main-*` presets need
  tmux 3.5, and a string tmux cannot read is never sent. (#17, #69, #116)

### Compatibility

- tmux 3.3a is the oldest supported release. A daemon older than that,
  including 3.3, and one whose version cannot be read or is an undated
  `master`, is refused with `FailureKind::unsupported` before the command has
  any effect, whatever the client executable reports. Release candidates are
  not supported: `X.Y-rcN` parses and ranks below `X.Y`. (#68, #120)
- `Server::check_release` refuses a release below the floor as the first
  command would, and sends nothing. Use it before launching tmux outside the
  library. (#68)
- Every command to a live daemon carries `-N` and a check that fails the list
  unrun if another daemon has replaced it, so a vanished server is not
  restarted by the client binary. `kill-server` on a server that is already
  gone succeeds. (#68)
- A literal command is read the way tmux reads it, under its alias or any
  unique prefix and in each position of a `;` list, by `Server::run`, batches,
  chains, asynchronous submissions, `over_control` and `Connection::execute`.
  These are refused before dispatch: `break-pane` without `-n` on exactly
  tmux 3.7, which ends the server; `select-layout` with a string tmux cannot
  read, which ends tmux 3.3 and 3.3a; `refresh-client -l` below 3.6a;
  `delete-buffer -b` and `set-buffer -b -n` below 3.4; and a session or window
  name holding `:` or `.` on exactly 3.7. (#69, #94)
- A command inside a string tmux parses itself is sent as written: the body of
  `if-shell`, `run-shell -C`, a brace block, a hook, a key binding or a sourced
  file. Do not put a refused command in one. The README and
  `docs/compatibility.md` list the commands, releases and effects. (#69)
- `Buffer::remove` and the MCP `paste_text` staging buffer need tmux 3.4 and
  return `FailureKind::unsupported` before any buffer is touched. (#69)
- A split percentage is spelled `-l N%`, which every supported release
  accepts.
- `Version` orders `3.7 < 3.7a < 3.7b`, with `next-X.Y` and `X.Y-rcN` before
  `X.Y`. `parse_version` stops at the first line of `tmux -V` output and
  refuses a number that would overflow. (#4, #68)
- `capabilities.json` lists each optional feature's first tmux release, its
  APIs and the error type they return. (#69)

### Windows preview

- An experimental x64 desktop preview, built with Visual Studio 2022 against
  psmux 3.3.7 at a pinned commit and archive hash, answers exact read-only
  entity queries and declines the rest with `FailureKind::unsupported` and
  `not_started` delivery. `-S` socket paths, persistent control mode, clients,
  buffers, options and hooks, session and window creation, and pane input and
  capture are not served. Ask `Server::capabilities()` before choosing a
  workflow. (#4)
- `SocketError::path_unsupported` reports a socket path asked of psmux, and
  `kSocketPathLimit` states the platform's limit, zero where paths do not
  apply. A psmux session or window name ending in `.<digits>` is rejected so a
  target-shaped name cannot be misread as `session:window.pane`. (#4, #10)
- On psmux `Server::try_submit` returns after admission, and cancelling a
  command proven to have started reports `cancelled` with `indeterminate`
  delivery. (#10, #11)
- CI builds the preview with MSVC, installs it and consumes the package, and
  runs no psmux test; Windows and macOS are supported on a best-effort basis.
  (#82)

### Testing

- `libtmux::testing` installs behind `find_package(libtmux COMPONENTS testing)`
  on POSIX. `ScopedTmuxServer::start` gives a suite a private server on its own
  socket, with `SocketNamespace::consumer` naming whose it is and a
  `TeardownReport` collecting what teardown could not clean up.
- `ctest` runs every test with `TMUX`, `TMUX_PANE`, `LIBTMUX_SOCKET` and
  `LIBTMUX_SOCKET_PATH` unset and `TMUX_TMPDIR` set to `tmux-sandbox/` in the
  build tree, so a bare `tmux` command in a test reaches no server outside the
  run. The run fails, naming the sockets, when a test leaves a tmux server
  running there. A test binary started by hand refuses to run until it is set
  up the same way. (#127)
- CI tests the oldest and newest released tmux, 3.3a and 3.8, on a push and a
  pull request. A release, which runs `ci.yml` at full scope itself, and a
  `workflow_dispatch` with `scope: full` run the whole table, `master` and the
  refusal cells. A dispatch also takes `tmux-ref` (a release, tag, branch or
  commit), `cxx-compiler` and `tmate`. (#124)

### Build

- The core has no dependencies. C++23 uses `std::expected`, and C++20 with
  `LIBTMUX_CXX_STANDARD=20` uses a pinned `tl::expected`. Each lives in its own
  inline namespace, `v2_cxx23` or `v2_cxx20`, so objects built against one fail
  to link against the other and name the missing symbol. The toolchains are
  clang 17+, GCC 13+ and Visual Studio 2022 17.3+, with CMake 3.25.
- `BUILD_SHARED_LIBS` selects a static or shared library on POSIX, and Windows
  rejects a shared build at configure time. The install carries
  `LIBTMUX_VERSION_*` macros in `libtmux/version.hpp`, a CMake package config
  and a pkg-config file for consumers that do not use CMake. (#18)

### vcpkg

- This repository is a vcpkg git registry, so installing needs neither an
  overlay port nor a clone. The `libtmux` port has an `mcp` feature, and a
  release's baseline is the registry commit that follows its tag, which the
  release notes name. Windows support in the port is an opt-in transition that
  a release's tagged gates must clear, and the port declares static linkage
  there, so a dynamic triplet installs the static library. (#4)
- `python3 -m tools.vcpkg check` fails when the git-tree recorded for a port's
  declared version is not that port's git-tree at `HEAD`, which `x-add-version`
  does not report loudly enough to gate on. `probe --repository` resolves the
  port over the public URL, so the gate catches a commit that was never pushed.

### Examples

- `libtmux::workspace` in `examples/workspace` parses tmuxp documents with
  `parse_tmuxp` and builds them with `build` and `build_windows`. A
  `BeforeBuild` or `BuildObserver` callback receives a `BuildEvent` carrying the
  session, window and pane IDs and may return a `BuildStop` that keeps or rolls
  back what was built. `tmux-workspace load` builds through it. (#17)

### Documentation

- The public headers render as an API reference through Doxygen and Breathe,
  one page per header that declares something, and `docs/api.md` is generated
  from them. Declaration comments are `///`, which Doxygen reads. (#12)
- The README's C++ is quoted from `examples/05-readme.cpp` and
  `examples/08-quickstart.cpp`, which the suite compiles and runs against a
  real tmux.
- The README, `docs/workspace-cli.md`, `docs/mcp-server.md` and
  `docs/windows.md` describe the socket choice, and CONTRIBUTING has a `.env`
  recipe for a program run by hand. (#128)

## 0.1.0-alpha.14 (2026-10-25)

This alpha rejects borrowing from const temporaries and single-pass ranges.
These are source-breaking, ABI-compatible changes: keep a named owner alive
or select an owned result. Boolean fields compose directly with `&&` and `||`,
and string and boolean fields accept `!=`. Bounded control replies retain the
exact output prefix when a line exceeds the byte limit.

### Breaking

- `parse` now rejects const notification temporaries. Source-breaking;
  ABI-compatible. Keep the notification alive while using the parsed views. (#105)

  Before: `auto parsed = parse(make_const_notification());`
  After: `const auto held = make_const_notification(); auto parsed = parse(held);`

- `ReferenceRange`, `first` and `exactly_one` now require forward ranges that
  yield lvalue references; the borrowing helpers also reject const temporaries.
  Source-breaking; ABI-compatible. Keep a named forward range alive for
  borrowing, or select an owned result from a single-pass range. (#106)

  Before: `auto result = first(input);`
  After: `auto result = first_owned(input);` (likewise `exactly_one_owned`)

### Querying

- Boolean fields compose directly with `&&` and `||`; string and boolean
  fields accept `!=`. Additive API; expression storage and lowering are
  unchanged. (#110)

### Control mode

- `Parser` retains the exact reply prefix when its byte limit cuts a line,
  instead of skipping that line and retaining later output. (#107)

### Pane

- `wait_for_text` retains its last capture when tmux exits successfully without
  capture output during shutdown. A blank pane still replaces the previous
  output.

### Documentation

- Split the README capability table into short rows with the crash causes in
  `docs/compatibility.md`, state the nested-command limit with its releases and
  effects, and repeat it in the `run_shell_command` and `send_keys` tool
  descriptions. (#98)
- Record in `docs/compatibility.md` that exact tmux 3.7 answers a name with `:`
  or `.` with "invalid ... name" and creates nothing, link the clipboard cause
  from the nested-command table, and state the wrong-buffer effect once. (#99)

### Testing

- MCP echo tests now write foreign output when their wait reports progress.
  Writer failures stop the test before it checks echo matching. (#109)

- The control fuzzer now instruments its compiled parser for coverage, ASan
  and UBSan. A test rejects builds missing that instrumentation. (#108)

- Cover a wait whose capture fails with "server exited unexpectedly", so the
  wait ends as `pane_died` without a liveness probe. (#99)

## 0.1.0-alpha.13 (2026-10-24)

This alpha makes the tmux 3.3a floor safe to run on. Commands that crash or
mis-run older daemons are refused before dispatch, with the release that fixes
them named. Control mode is never attached below tmux 3.7, and
`wait_for_text` polls there and reports a pane or server that goes away
instead of failing raw. CI no longer runs macOS or psmux, which the release
still builds.

### Breaking

- `Server::control`, `control_with_options`, `over_control` and
  `Connection::connect` return a `ProtocolError` naming tmux 3.7, before any
  client attaches, on an older daemon. Behaviour-breaking; the 3.3a floor is
  unchanged. (#69)

  Before: `auto connection = *server.control("work");`
  After:  `if (*server.tmux_version() >= Version{3, 7}) { auto connection = *server.control("work"); } else { pane.wait_for_text("done"); }`

- `Buffer::remove` returns `FailureKind::unsupported` with
  `DeliveryStatus::not_started` on a daemon older than tmux 3.4, where
  `delete-buffer -b` deletes the top buffer if the name is missing.
  Behaviour-breaking. (#69)

  Before: `auto removed = buffer.remove();`
  After:  `if (*server.tmux_version() >= Version{3, 4}) { auto removed = buffer.remove(); } else { pane.paste(buffer, true); }`

### Control mode

- `Pane::wait_for_text` polls the pane on a daemon older than tmux 3.7 and
  never attaches a control client there. (#69)
- `Pane::wait_for_text` and `Server::wait_for_text` return a `WaitResult` with
  `pane_died` set, tmux's words in `reason` and the last capture in `text`,
  when the pane or its server goes away mid-wait. The control path and the
  polling path answer the same, and a control stream that drops while the pane
  lives keeps waiting by polling. (#69)
- `Pane::wait_for_text` and `Server::wait_for_text` return a `WaitResult` with
  `pane_died` set and the output so far in `text` when the server is killed
  mid-wait, instead of a raw transport error. (#69, #94)
- `Connection::execute` refuses a command that `Server::run` refuses, such as
  an unnamed `break-pane` on exactly tmux 3.7, which ends the server. The
  refusal is a `connection_error` with `DeliveryStatus::not_started`, and the
  connection stays usable. (#69)

### Server

- `Server::run` and batches refuse `delete-buffer -b` and
  `set-buffer -b -n` on a daemon older than tmux 3.4, under any alias or
  unique prefix and anywhere in a `;` list. (#69)
- `Server::run` and batches refuse a `select-layout` whose layout the
  daemon cannot read, including the mirrored presets before tmux 3.5 and JSON
  layouts before 3.8-rc2. tmux 3.3 and 3.3a end their server on such a
  layout. (#69)
- `Server::run` and batches refuse a `break-pane` that names no window
  on exactly tmux 3.7, under any alias or unique prefix and anywhere in a `;`
  list, since that shape crashes the server. `Pane::break_out` is the
  route. (#69)
- Session and window names holding `:` or `.` are rejected with
  `FailureKind::validation` before dispatch on exactly tmux 3.7, which refuses
  them itself and creates nothing. (#69)
- `Server::run` and batches refuse a literal `new-session`,
  `new-window`, `rename-session` or `rename-window` that names a session or
  window with `:` or `.` on exactly tmux 3.7. (#69)
- `Server::run` and batches refuse a `refresh-client -l` clipboard query on a
  daemon older than tmux 3.6a, under any alias or unique prefix and anywhere in
  a `;` list, since it ends a tmux 3.3a server that has a client attached, and
  a 3.4 through 3.6 server when that client leaves before answering. (#69, #94)
- A `CommandExecutor` transport meets the same literal-command checks as a
  launched command. (#69)
- The literal-command checks do not read a command inside a string tmux parses
  itself: an `if-shell` body, `run-shell -C`, a brace block, a hook, a key
  binding or a sourced file. Do not put a refused command in one, because the
  daemon will run it. The README and `docs/compatibility.md` list the commands,
  releases and effects. (#69)

### Support

- Add `capabilities.json`, which lists each optional feature's first tmux
  release, its APIs and the error type they return, for the documentation
  site's capability pages. (#69)

### Testing

- CI no longer runs macOS or psmux. The `macos` job leaves `ci.yml`, the
  `windows` job keeps its MSVC build, install and package steps but no
  longer starts psmux, and `release.yml` drops `registry-macos` and the
  psmux steps of `server-windows`, so `all-checks-passed` and `publish` wait
  on no macOS or psmux run. Release builds still publish the macOS and
  Windows binaries, and the README, `docs/windows.md` and `CONTRIBUTING.md`
  state that both are supported on a best-effort basis with highly limited
  CI coverage. (#82)

## 0.1.0-alpha.12 (2026-10-18)

This alpha requires tmux 3.3a. Every command is checked against the daemon it
reaches, not the client executable, before it has any effect, and an older or
unrecognized daemon is refused with `FailureKind::unsupported` and the remedy.
The check fires no user hook, and a daemon replaced between commands runs
nothing. The workspace CLI reports the refusal as `tmux_unsupported`, and the
MCP server refuses at startup and names the floor.

### Breaking

- tmux 3.3a is the oldest supported release. A daemon older than 3.3a,
  including 3.3 itself, `next-3.3a` and an undated `master`, is refused
  with `FailureKind::unsupported` before the command has any effect,
  whatever the client executable reports. Upgrade tmux and restart the
  server, or stay on the previous libtmux release. Behaviour-breaking. (#68)
- A daemon or client older than 3.2 is refused the same way: 3.0a answers
  "server version is too old for client", 3.1c has no `run-shell -C`, and
  either client rejects `-N`. The refusal returns at once, although those
  daemons keep the client's stdout open after it exits. (#68)
- `Version::candidate` orders a release candidate below its release, so a
  gate names the earliest candidate that has the behaviour: JSON saved layouts
  need `3.8-rc2` or newer and are refused on `3.8-rc`, written
  `.candidate = 2`. `parse_version` no longer reads `master`, `X.Y-master`,
  `X.Y-dev` or a `next-` build with a patch letter; tmux reports none of
  them. Source-breaking. (#68)
- Every command to a live daemon is sent with `-N` and a check that fails
  the list unrun if another daemon has replaced it, so a vanished server is
  not restarted by the client binary. `Session::attach_command` carries
  `-N` too. Raw client flags such as `-c` are no longer dispatched, and
  `kill-server` on a server that is already gone succeeds. Behaviour-breaking.
  (#68)

### Server

- Add `Server::check_release`, which refuses a release below the floor as the
  first command would and sends nothing. Use it before launching tmux outside
  the library. (#68)
- `Server::try_submit` asks the daemon for its release before admission, so a
  submission to an endpoint with no answering daemon can block on that probe
  for up to the command's timeout. (#68)

### Testing

- Add `LIBTMUX_REQUIRES_TMUX_CANDIDATE` for a capability that a release
  candidate already has. The control-mode suite refuses to start when the
  tmux it was built against differs from the tmux on `PATH`. (#68)

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
- No dependencies in the core.
