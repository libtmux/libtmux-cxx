# Changelog

What changed in each release, for someone deciding whether to take it. The
conventions are in [`.github/WRITING.md`](.github/WRITING.md#changelog).

The `0.1.0-alpha.1` and `0.1.0-alpha.2` entries were written after those
releases, from the commits each tag carries. Everything from `0.1.0-alpha.3` on
was recorded as it landed.

## Unreleased

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
