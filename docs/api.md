# API reference

The full surface targets tmux on POSIX. Windows uses the bounded psmux
preview documented in the project README; unsupported operations fail
before dispatch rather than approximating tmux semantics.

Generated from the headers by `tools/docs/api_index.py`; the prose here
is the prose there. Run it with `--check` to prove this page is current.

## Headers

- [`libtmux/libtmux.hpp`](#libtmux-libtmux-hpp)
- [`libtmux/server.hpp`](#libtmux-server-hpp)
- [`libtmux/capabilities.hpp`](#libtmux-capabilities-hpp)
- [`libtmux/entities.hpp`](#libtmux-entities-hpp)
- [`libtmux/snapshot.hpp`](#libtmux-snapshot-hpp)
- [`libtmux/filter_expr.hpp`](#libtmux-filter-expr-hpp)
- [`libtmux/relations.hpp`](#libtmux-relations-hpp)
- [`libtmux/cardinality.hpp`](#libtmux-cardinality-hpp)
- [`libtmux/command.hpp`](#libtmux-command-hpp)
- [`libtmux/options.hpp`](#libtmux-options-hpp)
- [`libtmux/control.hpp`](#libtmux-control-hpp)
- [`libtmux/batch.hpp`](#libtmux-batch-hpp)
- [`libtmux/chain.hpp`](#libtmux-chain-hpp)
- [`libtmux/keys.hpp`](#libtmux-keys-hpp)
- [`libtmux/capture.hpp`](#libtmux-capture-hpp)
- [`libtmux/target.hpp`](#libtmux-target-hpp)
- [`libtmux/socket.hpp`](#libtmux-socket-hpp)
- [`libtmux/format.hpp`](#libtmux-format-hpp)
- [`libtmux/version.hpp`](#libtmux-version-hpp)
- [`libtmux/lowering.hpp`](#libtmux-lowering-hpp)
- [`libtmux/lowered_node.hpp`](#libtmux-lowered-node-hpp)
- [`libtmux/legacy_lookup.hpp`](#libtmux-legacy-lookup-hpp)
- [`libtmux/expected.hpp`](#libtmux-expected-hpp)
- [`libtmux/abi.hpp`](#libtmux-abi-hpp)

<a id="libtmux-libtmux-hpp"></a>
## `libtmux/libtmux.hpp`

libtmux: a typed C++ interface to tmux.  This umbrella header pulls in the dependency-free core: value types for the things tmux prints, snapshots that own what a command returned, entities projected from those snapshots, and expressions for selecting among them. Nothing here spawns a process; execution belongs to the connection type.

<a id="libtmux-server-hpp"></a>
## `libtmux/server.hpp`

The connection root.  A Server names which tmux server to talk to and how to reach it. It is a handle: copying one costs a reference count, and every entity taken from it keeps the same connection alive, so a listing outlives the Server value that produced it.  No transport type appears in this header. `run` is declared here and defined against a private backend, so an async or control-mode executor can replace that backend without changing a caller or breaking ABI.

**Symbols:**

- [`Server`](#libtmux-server-hpp-server)
  - [`Server::at_socket_path`](#libtmux-server-hpp-server-at-socket-path)
  - [`Server::at_socket_name`](#libtmux-server-hpp-server-at-socket-name)
  - [`Server::from_env`](#libtmux-server-hpp-server-from-env)
  - [`Server::at_default`](#libtmux-server-hpp-server-at-default)
  - [`Server::capabilities`](#libtmux-server-hpp-server-capabilities)
  - [`Server::run`](#libtmux-server-hpp-server-run)
  - [`Server::run_batch`](#libtmux-server-hpp-server-run-batch)
  - [`Server::run_chain`](#libtmux-server-hpp-server-run-chain)
  - [`Server::control`](#libtmux-server-hpp-server-control)
  - [`Server::control_with_options`](#libtmux-server-hpp-server-control-with-options)
  - [`Server::take_notifications`](#libtmux-server-hpp-server-take-notifications)
  - [`Server::dropped_notifications`](#libtmux-server-hpp-server-dropped-notifications)
  - [`Server::over_control`](#libtmux-server-hpp-server-over-control)
  - [`Server::over_control_with_options`](#libtmux-server-hpp-server-over-control-with-options)
  - [`Server::tmux_version`](#libtmux-server-hpp-server-tmux-version)
  - [`Server::is_alive`](#libtmux-server-hpp-server-is-alive)
  - [`Server::check_alive`](#libtmux-server-hpp-server-check-alive)
  - [`Server::kill`](#libtmux-server-hpp-server-kill)
  - [`Server::sessions`](#libtmux-server-hpp-server-sessions)
  - [`Server::windows`](#libtmux-server-hpp-server-windows)
  - [`Server::panes`](#libtmux-server-hpp-server-panes)
  - [`Server::clients`](#libtmux-server-hpp-server-clients)
  - [`Server::wait_for`](#libtmux-server-hpp-server-wait-for)
  - [`Server::signal`](#libtmux-server-hpp-server-signal)
  - [`Server::commands`](#libtmux-server-hpp-server-commands)
  - [`Server::buffers`](#libtmux-server-hpp-server-buffers)
  - [`Server::load_buffer`](#libtmux-server-hpp-server-load-buffer)
  - [`Server::save_buffer`](#libtmux-server-hpp-server-save-buffer)
  - [`Server::bind_key`](#libtmux-server-hpp-server-bind-key)
  - [`Server::unbind_key`](#libtmux-server-hpp-server-unbind-key)
  - [`Server::run_shell`](#libtmux-server-hpp-server-run-shell)
  - [`Server::source_file`](#libtmux-server-hpp-server-source-file)
  - [`Server::check_file`](#libtmux-server-hpp-server-check-file)
  - [`Server::expand`](#libtmux-server-hpp-server-expand)
  - [`Server::show_message`](#libtmux-server-hpp-server-show-message)
  - [`Server::set_buffer`](#libtmux-server-hpp-server-set-buffer)
  - [`Server::session`](#libtmux-server-hpp-server-session)
  - [`Server::window`](#libtmux-server-hpp-server-window)
  - [`Server::pane`](#libtmux-server-hpp-server-pane)
  - [`Server::new_session`](#libtmux-server-hpp-server-new-session)
  - [`Server::new_session`](#libtmux-server-hpp-server-new-session-2)
  - [`Server::options`](#libtmux-server-hpp-server-options)
  - [`Server::server_options`](#libtmux-server-hpp-server-server-options)
  - [`Server::set_server_option`](#libtmux-server-hpp-server-set-server-option)
  - [`Server::global_options`](#libtmux-server-hpp-server-global-options)
  - [`Server::set_global_option`](#libtmux-server-hpp-server-set-global-option)
  - [`Server::hooks`](#libtmux-server-hpp-server-hooks)
  - [`Server::global_hooks`](#libtmux-server-hpp-server-global-hooks)
  - [`Server::set_global_hook`](#libtmux-server-hpp-server-set-global-hook)

<a id="libtmux-server-hpp-server"></a>
### `Server`

```cpp
class Server;
```

<a id="libtmux-server-hpp-server-at-socket-path"></a>
#### `Server::at_socket_path`

```cpp
[[nodiscard]] static expected<Server, CommandFailure> at_socket_path(std::string_view path, CommandObserver observer = {}, ExecutionPolicy policy = {});
```
`-S path`: the socket file, used verbatim.  These report `CommandFailure`, the same type every other call reports, rather than the `SocketError` the argument builders use: a factory that failed differently is a factory nothing can be chained onto. The reason a selector was rejected is in the diagnostic, and `socket_path_arguments` still returns the enum for a caller that wants to branch on it.  An observer, if given, is told about every command this server runs. It is fixed at construction because the connection is immutable afterwards, and that is what makes a Server safe to copy between threads. The policy is fixed for the same reason, and says what a call gets when it names no timeout or limit of its own.

<a id="libtmux-server-hpp-server-at-socket-name"></a>
#### `Server::at_socket_name`

```cpp
[[nodiscard]] static expected<Server, CommandFailure> at_socket_name(std::string_view name, CommandObserver observer = {}, ExecutionPolicy policy = {});
```
`-L name`: resolved under tmux's socket directory, as the tmux flag does.

<a id="libtmux-server-hpp-server-from-env"></a>
#### `Server::from_env`

```cpp
[[nodiscard]] static expected<Server, CommandFailure> from_env(CommandObserver observer = {}, ExecutionPolicy policy = {});
```
The server this process is running inside.  tmux exports `TMUX` to everything it starts, as `<socket path>,<server pid>,<session id>`. Only the socket path is read: the session id is stale the moment a pane moves, and a `#()` job carries no session at all — so a caller who wants the session asks tmux, rather than trusting what it inherited.

<a id="libtmux-server-hpp-server-at-default"></a>
#### `Server::at_default`

```cpp
[[nodiscard]] static expected<Server, CommandFailure> at_default(CommandObserver observer = {}, ExecutionPolicy policy = {});
```
The server tmux would talk to with no `-L` or `-S` at all, which is the one a person means when they say "my tmux".

<a id="libtmux-server-hpp-server-capabilities"></a>
#### `Server::capabilities`

```cpp
[[nodiscard]] ServerCapabilities capabilities() const noexcept;
```
The local backend contract; no command runs. `tmux_version()` separately probes the executable on PATH.

<a id="libtmux-server-hpp-server-run"></a>
#### `Server::run`

```cpp
[[nodiscard]] expected<std::string, CommandFailure> run(const std::vector<std::string>& command, std::optional<std::chrono::milliseconds> timeout = {}, std::optional<std::size_t> output_limit = {}) const;
```
Run one command and return its standard output.  The timeout still rides on the call: how long a caller will wait is a property of what they asked for, and listing sessions does not share a deadline with attaching a client. Unset takes the server's `ExecutionPolicy`, which is thirty seconds rather than forever — a floor, not a guess at what this particular command needs. `output_limit` bounds how much of tmux's answer this call will hold. Past it the command reports `truncated` rather than returning a prefix that reads like a complete answer. Unset uses the package default, which is ample for every listing and can be too small for a long scrollback.

<a id="libtmux-server-hpp-server-run-batch"></a>
#### `Server::run_batch`

```cpp
[[nodiscard]] expected<std::string, CommandFailure> run_batch(const CommandBatch& batch) const;
```
Run several commands in one invocation. tmux runs a batch fail-fast, so a failed batch is partially applied rather than rolled back, and one exit status covers the group: which member failed needs control mode.

<a id="libtmux-server-hpp-server-run-chain"></a>
#### `Server::run_chain`

```cpp
[[nodiscard]] expected<std::string, CommandFailure> run_chain(const Chain& chain) const;
```
Run a chain. A chain that failed validation never reaches tmux, and says which step was wrong rather than surfacing a tmux message about it.

<a id="libtmux-server-hpp-server-control"></a>
#### `Server::control`

```cpp
[[nodiscard]] expected<Connection, ProtocolError> control(std::string_view session) const;
```
Open a control-mode connection to one session.  This is the streaming half of the transport: a control connection stays open, gives each command its own reply block, and delivers asynchronous notifications between them. The synchronous surface above is unaffected — a caller who never opens one never pays for it.  Fails with `ProtocolError`, not `CommandFailure`, because that is what the `Connection` it returns speaks: an error type here that the value's own surface does not use would make the doorway disagree with the room. `over_control` returns an ordinary `Server` and so reports the ordinary failure.

<a id="libtmux-server-hpp-server-control-with-options"></a>
#### `Server::control_with_options`

```cpp
[[nodiscard]] expected<Connection, ProtocolError> control_with_options(std::string_view session, ConnectionOptions options) const;
```
The Server supplies the socket and `session` supplies the session name; every other connection option is kept, including pane output policy.

<a id="libtmux-server-hpp-server-take-notifications"></a>
#### `Server::take_notifications`

```cpp
[[nodiscard]] std::vector<Notification> take_notifications() const;
```
What tmux has said on its own initiative since the last call: a window renamed, a pane exited, a client attached. A Server that runs a process per command hears nothing between them and answers with nothing, so a caller that wants this opens one with `over_control`.  The buffer is bounded; `dropped_notifications` says how many were discarded, which distinguishes a quiet server from one that outran a caller who was not collecting.

<a id="libtmux-server-hpp-server-dropped-notifications"></a>
#### `Server::dropped_notifications`

```cpp
[[nodiscard]] std::size_t dropped_notifications() const noexcept;
```

<a id="libtmux-server-hpp-server-over-control"></a>
#### `Server::over_control`

```cpp
[[nodiscard]] expected<Server, CommandFailure> over_control(std::string_view session) const;
```
The same surface, dispatched over one open control connection instead of a process per command. Entities taken from the result are ordinary entities; nothing above the transport knows the difference.  A connection carries one conversation, so commands over it are serialized. Two Servers over the same socket are two conversations.

<a id="libtmux-server-hpp-server-over-control-with-options"></a>
#### `Server::over_control_with_options`

```cpp
[[nodiscard]] expected<Server, CommandFailure> over_control_with_options(std::string_view session, ConnectionOptions options) const;
```
As above, with the connection's timeouts, limits, executable, and output policy selected by the caller rather than reconstructed from a route.

<a id="libtmux-server-hpp-server-tmux-version"></a>
#### `Server::tmux_version`

```cpp
[[nodiscard]] expected<Version, CommandFailure> tmux_version() const;
```
Which tmux is behind this connection. `tmux -V` answers without touching the server, so this reports a version even when nothing is running.

<a id="libtmux-server-hpp-server-is-alive"></a>
#### `Server::is_alive`

```cpp
[[nodiscard]] bool is_alive(std::chrono::milliseconds timeout = std::chrono::seconds{ 5}) const;
```
Whether a server is answering on this socket. False covers every reason — no server, no socket, a tmux that would not run — because a caller who only wants to know whether to start one does not need to tell them apart.  Bounded by default: the one call whose whole job is answering "can I reach this" must not be the call that hangs. A stalled socket or a stopped server answers no, in time, rather than never.

<a id="libtmux-server-hpp-server-check-alive"></a>
#### `Server::check_alive`

```cpp
[[nodiscard]] expected<void, CommandFailure> check_alive(std::chrono::milliseconds timeout = std::chrono::seconds{5}) const;
```
The same question, keeping why the answer was no.

<a id="libtmux-server-hpp-server-kill"></a>
#### `Server::kill`

```cpp
[[nodiscard]] expected<void, CommandFailure> kill() const;
```
End the server and everything in it.

<a id="libtmux-server-hpp-server-sessions"></a>
#### `Server::sessions`

```cpp
[[nodiscard]] expected<std::vector<Session>, CommandFailure> sessions() const;
```
One snapshot each. Iterating or filtering the result never reaches tmux again; taking a current view means calling these again.

<a id="libtmux-server-hpp-server-windows"></a>
#### `Server::windows`

```cpp
[[nodiscard]] expected<std::vector<Window>, CommandFailure> windows() const;
```
tmux scopes window and pane listings to the current session unless asked for every one, so `-a` is part of the request rather than a caller's responsibility to remember.

<a id="libtmux-server-hpp-server-panes"></a>
#### `Server::panes`

```cpp
[[nodiscard]] expected<std::vector<Pane>, CommandFailure> panes() const;
```

<a id="libtmux-server-hpp-server-clients"></a>
#### `Server::clients`

```cpp
[[nodiscard]] expected<std::vector<Client>, CommandFailure> clients() const;
```

<a id="libtmux-server-hpp-server-wait-for"></a>
#### `Server::wait_for`

```cpp
[[nodiscard]] expected<void, CommandFailure> wait_for(std::string_view channel, std::optional<std::chrono::milliseconds> timeout = {}) const;
```
Block until someone signals this channel, or the deadline passes.  tmux latches a signal: one sent while nobody is waiting satisfies the next wait rather than being lost. That makes signal-before-wait safe, and it also means a stale signal can release a later waiter, so a channel is worth naming for one exchange rather than reusing.  A server that dies under a waiter makes tmux exit zero, which is indistinguishable from being signalled — a caller would carry on as though the other side had spoken. This reports that as a failure instead, which is the reason to prefer it over running the command.

<a id="libtmux-server-hpp-server-signal"></a>
#### `Server::signal`

```cpp
[[nodiscard]] expected<void, CommandFailure> signal(std::string_view channel) const;
```
Release whoever is waiting on the channel, or latch it for whoever waits next.

<a id="libtmux-server-hpp-server-commands"></a>
#### `Server::commands`

```cpp
[[nodiscard]] expected<std::vector<Command>, CommandFailure> commands() const;
```
Every command this tmux understands, with its alias and usage.  Asking beats inferring: the supported-version range is a floor, not a description, and a distribution can ship a build with commands left out. A caller deciding whether a capability exists can look.

<a id="libtmux-server-hpp-server-buffers"></a>
#### `Server::buffers`

```cpp
[[nodiscard]] expected<std::vector<Buffer>, CommandFailure> buffers() const;
```
The server's cut buffers, newest first as tmux orders them. A server holding none answers with an empty list rather than a failure, like every other listing here.

<a id="libtmux-server-hpp-server-load-buffer"></a>
#### `Server::load_buffer`

```cpp
[[nodiscard]] expected<void, CommandFailure> load_buffer(std::string_view name, const std::filesystem::path& from) const;
```
Read a file into a named buffer, and write one back out.  The file is read and written by the tmux server, so the path is the server's to resolve — which matters when it is not on this machine. The bytes are not interpreted: a buffer round-tripped through a file comes back identical.

<a id="libtmux-server-hpp-server-save-buffer"></a>
#### `Server::save_buffer`

```cpp
[[nodiscard]] expected<void, CommandFailure> save_buffer(std::string_view name, const std::filesystem::path& to) const;
```

<a id="libtmux-server-hpp-server-bind-key"></a>
#### `Server::bind_key`

```cpp
[[nodiscard]] expected<void, CommandFailure> bind_key(std::string_view table, std::string_view key, const std::vector<std::string>& command, bool repeatable = false) const;
```
Bind a key in a key table, and take a binding away again.  The command is argv, not a string, so nothing here has to be quoted for tmux to take it apart again correctly.  A table name containing whitespace is refused. tmux accepts one and then prints it unquoted in `list-keys`, where `-T my table X command` cannot be told apart from the table `my` bound to the key `table` — so a name that survives being listed is required, the same way a target refuses a name that cannot survive being parsed.  Key names are tmux's to check: unlike `send-keys`, `bind-key` reports an unknown one — including an empty one — so there is nothing for this to add, and nothing here repeats it.  `repeatable` is tmux's `-r`, letting the key repeat without the prefix being pressed again.

<a id="libtmux-server-hpp-server-unbind-key"></a>
#### `Server::unbind_key`

```cpp
[[nodiscard]] expected<void, CommandFailure> unbind_key(std::string_view table, std::string_view key) const;
```
Unbinding a key that was not bound succeeds; unbinding in a table that does not exist is refused. A table exists only while something is bound in it, so taking away the last binding takes the table with it.

<a id="libtmux-server-hpp-server-run-shell"></a>
#### `Server::run_shell`

```cpp
[[nodiscard]] expected<void, CommandFailure> run_shell(std::string_view command, bool background = false) const;
```
Run a shell command on the machine the server is on.  Reports whether it ran, not what it printed. tmux hands back the output on most versions and discards it on 3.3a and 3.4, and an answer that is empty on two versions in the middle of the range is worse than no answer at all. A caller that needs the output redirects it to a file.  What is uniform is the exit status: a command that fails is a failure here, carrying its code in `exit_code`.  `background` is tmux's `-b`, which returns as soon as the command is started. Nothing can then be said about how it ended, so a backgrounded command that fails still reports success.

<a id="libtmux-server-hpp-server-source-file"></a>
#### `Server::source_file`

```cpp
[[nodiscard]] expected<void, CommandFailure> source_file(const std::filesystem::path& file) const;
```
Run the commands in a file, the way tmux runs a configuration file.  The server reads the file, so the path is the server's to resolve. A file it cannot read is reported rather than passed over.

<a id="libtmux-server-hpp-server-check-file"></a>
#### `Server::check_file`

```cpp
[[nodiscard]] expected<void, CommandFailure> check_file(const std::filesystem::path& file) const;
```
Parse the same file and report what tmux would refuse in it, running none of it. This is how a program checks a configuration it is about to apply without applying half of it first.

<a id="libtmux-server-hpp-server-expand"></a>
#### `Server::expand`

```cpp
[[nodiscard]] expected<std::string, CommandFailure> expand(std::string_view format) const;
```
Ask tmux to expand a format, against no target but the server itself.  Unguarded, unlike the entity forms: there is no target here that could have gone away. A server kept alive with no sessions still answers its own fields, and answers the session-scoped ones with nothing — which is the truth about that server, not a failure to report.

<a id="libtmux-server-hpp-server-show-message"></a>
#### `Server::show_message`

```cpp
[[nodiscard]] expected<void, CommandFailure> show_message(std::string_view text) const;
```
Put a message on the status line of every attached client, and send it to each control client as `%message`.  tmux expands the text as a format, so a `#{...}` in it is substituted rather than shown. Text built from data belongs in `escape_literal` first.

<a id="libtmux-server-hpp-server-set-buffer"></a>
#### `Server::set_buffer`

```cpp
[[nodiscard]] expected<void, CommandFailure> set_buffer(std::string_view name, std::string_view data) const;
```
Put text in a named buffer, replacing what was there. An empty name lets tmux choose one, which is what a caller copying without caring about the name wants.

<a id="libtmux-server-hpp-server-session"></a>
#### `Server::session`

```cpp
[[nodiscard]] expected<Session, CommandFailure> session(std::string_view target) const;
```
One object by target, for a caller holding an id or a `session:window` path that came from somewhere else. The target is resolved the way tmux resolves it, so a session target names that session's active pane and the window that pane is in. A target tmux cannot resolve is reported missing.

<a id="libtmux-server-hpp-server-window"></a>
#### `Server::window`

```cpp
[[nodiscard]] expected<Window, CommandFailure> window(std::string_view target) const;
```

<a id="libtmux-server-hpp-server-pane"></a>
#### `Server::pane`

```cpp
[[nodiscard]] expected<Pane, CommandFailure> pane(std::string_view target) const;
```

<a id="libtmux-server-hpp-server-new-session"></a>
#### `Server::new_session`

```cpp
[[nodiscard]] expected<Session, CommandFailure> new_session(std::string_view name) const;
```
Created detached, and returned, because tmux prints what it made. Windows psmux rejects typed creation: concurrent creators cannot prove ownership.

<a id="libtmux-server-hpp-server-new-session-2"></a>
#### `Server::new_session`

```cpp
[[nodiscard]] expected<Session, CommandFailure> new_session(NewSessionOptions options) const;
```

<a id="libtmux-server-hpp-server-options"></a>
#### `Server::options`

```cpp
[[nodiscard]] expected<std::vector<OptionEntry>, CommandFailure> options(std::string_view target = {}) const;
```

<a id="libtmux-server-hpp-server-server-options"></a>
#### `Server::server_options`

```cpp
[[nodiscard]] expected<std::vector<OptionEntry>, CommandFailure> server_options() const;
```
The server's own options, which are neither session nor window options and are the only ones a server without a session still has.

<a id="libtmux-server-hpp-server-set-server-option"></a>
#### `Server::set_server_option`

```cpp
[[nodiscard]] expected<void, CommandFailure> set_server_option(std::string_view name, std::string_view value) const;
```

<a id="libtmux-server-hpp-server-global-options"></a>
#### `Server::global_options`

```cpp
[[nodiscard]] expected<std::vector<OptionEntry>, CommandFailure> global_options() const;
```

<a id="libtmux-server-hpp-server-set-global-option"></a>
#### `Server::set_global_option`

```cpp
[[nodiscard]] expected<void, CommandFailure> set_global_option(std::string_view name, std::string_view value) const;
```
Sets the value every session inherits, rather than one session's own.

<a id="libtmux-server-hpp-server-hooks"></a>
#### `Server::hooks`

```cpp
[[nodiscard]] expected<std::vector<OptionEntry>, CommandFailure> hooks(std::string_view target = {}) const;
```

<a id="libtmux-server-hpp-server-global-hooks"></a>
#### `Server::global_hooks`

```cpp
[[nodiscard]] expected<std::vector<OptionEntry>, CommandFailure> global_hooks() const;
```
A hook set globally is not reported by the unscoped listing, so reading it back needs the scope it was set with.

<a id="libtmux-server-hpp-server-set-global-hook"></a>
#### `Server::set_global_hook`

```cpp
[[nodiscard]] expected<void, CommandFailure> set_global_hook(std::string_view name, std::string_view command) const;
```

<a id="libtmux-capabilities-hpp"></a>
## `libtmux/capabilities.hpp`

What this Server can promise without probing tmux.  These describe the local backend contract, not the executable on PATH. Unknown custom backends report no features, so checks fail closed.

**Symbols:**

- [`ServerImplementation`](#libtmux-capabilities-hpp-serverimplementation)
  - [`ServerImplementation::unknown`](#libtmux-capabilities-hpp-serverimplementation-unknown)
  - [`ServerImplementation::tmux`](#libtmux-capabilities-hpp-serverimplementation-tmux)
  - [`ServerImplementation::psmux`](#libtmux-capabilities-hpp-serverimplementation-psmux)
- [`BackendKind`](#libtmux-capabilities-hpp-backendkind)
  - [`BackendKind::custom`](#libtmux-capabilities-hpp-backendkind-custom)
  - [`BackendKind::subprocess`](#libtmux-capabilities-hpp-backendkind-subprocess)
  - [`BackendKind::control_mode`](#libtmux-capabilities-hpp-backendkind-control-mode)
- [`ServerFeature`](#libtmux-capabilities-hpp-serverfeature)
  - [`ServerFeature::exact_inspection`](#libtmux-capabilities-hpp-serverfeature-exact-inspection)
  - [`ServerFeature::server_cleanup`](#libtmux-capabilities-hpp-serverfeature-server-cleanup)
  - [`ServerFeature::server_entity_lookup`](#libtmux-capabilities-hpp-serverfeature-server-entity-lookup)
  - [`ServerFeature::session_creation`](#libtmux-capabilities-hpp-serverfeature-session-creation)
  - [`ServerFeature::window_creation`](#libtmux-capabilities-hpp-serverfeature-window-creation)
  - [`ServerFeature::captured_mutation`](#libtmux-capabilities-hpp-serverfeature-captured-mutation)
  - [`ServerFeature::pane_io`](#libtmux-capabilities-hpp-serverfeature-pane-io)
  - [`ServerFeature::terminal_attach`](#libtmux-capabilities-hpp-serverfeature-terminal-attach)
  - [`ServerFeature::reusable_window_target`](#libtmux-capabilities-hpp-serverfeature-reusable-window-target)
  - [`ServerFeature::server_state`](#libtmux-capabilities-hpp-serverfeature-server-state)
  - [`ServerFeature::wait_channels`](#libtmux-capabilities-hpp-serverfeature-wait-channels)
  - [`ServerFeature::control_mode`](#libtmux-capabilities-hpp-serverfeature-control-mode)
  - [`ServerFeature::receives_asynchronous_notifications`](#libtmux-capabilities-hpp-serverfeature-receives-asynchronous-notifications)
- [`ServerCapabilities`](#libtmux-capabilities-hpp-servercapabilities)
  - [`ServerCapabilities::implementation`](#libtmux-capabilities-hpp-servercapabilities-implementation)
  - [`ServerCapabilities::backend`](#libtmux-capabilities-hpp-servercapabilities-backend)
  - [`ServerCapabilities::supports`](#libtmux-capabilities-hpp-servercapabilities-supports)
- [`Free symbols`](#libtmux-capabilities-hpp-free-symbols)
  - [`to_string`](#libtmux-capabilities-hpp-free-symbols-to-string)
  - [`to_string`](#libtmux-capabilities-hpp-free-symbols-to-string-2)
  - [`to_string`](#libtmux-capabilities-hpp-free-symbols-to-string-3)

<a id="libtmux-capabilities-hpp-serverimplementation"></a>
### `ServerImplementation`

```cpp
enum class ServerImplementation;
```

<a id="libtmux-capabilities-hpp-serverimplementation-unknown"></a>
#### `ServerImplementation::unknown` — `unknown,`

<a id="libtmux-capabilities-hpp-serverimplementation-tmux"></a>
#### `ServerImplementation::tmux` — `tmux,`

<a id="libtmux-capabilities-hpp-serverimplementation-psmux"></a>
#### `ServerImplementation::psmux` — `psmux,`

<a id="libtmux-capabilities-hpp-backendkind"></a>
### `BackendKind`

```cpp
enum class BackendKind;
```

<a id="libtmux-capabilities-hpp-backendkind-custom"></a>
#### `BackendKind::custom` — `custom,`

<a id="libtmux-capabilities-hpp-backendkind-subprocess"></a>
#### `BackendKind::subprocess` — `subprocess,`

<a id="libtmux-capabilities-hpp-backendkind-control-mode"></a>
#### `BackendKind::control_mode` — `control_mode,`

<a id="libtmux-capabilities-hpp-serverfeature"></a>
### `ServerFeature`

Coarse-grained promises callers choose around. Raw commands remain unchecked; the psmux preview promises only exact inspection and namespace cleanup.

```cpp
enum class ServerFeature;
```

<a id="libtmux-capabilities-hpp-serverfeature-exact-inspection"></a>
#### `ServerFeature::exact_inspection` — `exact_inspection,`

Exact session/window/pane listings, traversal, refresh, and format reads.

<a id="libtmux-capabilities-hpp-serverfeature-server-cleanup"></a>
#### `ServerFeature::server_cleanup` — `server_cleanup,`

Cleanup of the selected namespace under the Server's execution policy.

<a id="libtmux-capabilities-hpp-serverfeature-server-entity-lookup"></a>
#### `ServerFeature::server_entity_lookup` — `server_entity_lookup,`

`Server::window` and `Server::pane` without an owning session handle.

<a id="libtmux-capabilities-hpp-serverfeature-session-creation"></a>
#### `ServerFeature::session_creation` — `session_creation,`

Typed session creation with an attributable result.

<a id="libtmux-capabilities-hpp-serverfeature-window-creation"></a>
#### `ServerFeature::window_creation` — `window_creation,`

Typed window creation with an attributable result.

<a id="libtmux-capabilities-hpp-serverfeature-captured-mutation"></a>
#### `ServerFeature::captured_mutation` — `captured_mutation,`

Mutating an object captured in an earlier snapshot.

<a id="libtmux-capabilities-hpp-serverfeature-pane-io"></a>
#### `ServerFeature::pane_io` — `pane_io,`

Pane input, capture, copy mode, history, and piping.

<a id="libtmux-capabilities-hpp-serverfeature-terminal-attach"></a>
#### `ServerFeature::terminal_attach` — `terminal_attach,`

Producing argv that attaches a caller-owned terminal to a session.

<a id="libtmux-capabilities-hpp-serverfeature-reusable-window-target"></a>
#### `ServerFeature::reusable_window_target` — `reusable_window_target,`

A reusable public target that identifies one captured window link.

<a id="libtmux-capabilities-hpp-serverfeature-server-state"></a>
#### `ServerFeature::server_state` — `server_state,`

Clients, buffers, commands, configuration, options, and hooks.

<a id="libtmux-capabilities-hpp-serverfeature-wait-channels"></a>
#### `ServerFeature::wait_channels` — `wait_channels,`

Latched `wait-for` channels.

<a id="libtmux-capabilities-hpp-serverfeature-control-mode"></a>
#### `ServerFeature::control_mode` — `control_mode,`

This Server can open a persistent control connection.

<a id="libtmux-capabilities-hpp-serverfeature-receives-asynchronous-notifications"></a>
#### `ServerFeature::receives_asynchronous_notifications` — `receives_asynchronous_notifications,`

This Server itself is already backed by control mode and receives events.

<a id="libtmux-capabilities-hpp-servercapabilities"></a>
### `ServerCapabilities`

```cpp
struct ServerCapabilities;
```

<a id="libtmux-capabilities-hpp-servercapabilities-implementation"></a>
#### `ServerCapabilities::implementation`

```cpp
ServerImplementation implementation{ServerImplementation::unknown};
```

<a id="libtmux-capabilities-hpp-servercapabilities-backend"></a>
#### `ServerCapabilities::backend`

```cpp
BackendKind backend{BackendKind::custom};
```

<a id="libtmux-capabilities-hpp-servercapabilities-supports"></a>
#### `ServerCapabilities::supports`

```cpp
[[nodiscard]] constexpr bool supports(ServerFeature feature) const noexcept;
```
Purely local: this never launches tmux or touches a server.

<a id="libtmux-capabilities-hpp-free-symbols"></a>
### `Free symbols`

<a id="libtmux-capabilities-hpp-free-symbols-to-string"></a>
#### `to_string`

```cpp
[[nodiscard]] constexpr std::string_view to_string(ServerImplementation implementation) noexcept;
```

<a id="libtmux-capabilities-hpp-free-symbols-to-string-2"></a>
#### `to_string`

```cpp
[[nodiscard]] constexpr std::string_view to_string(BackendKind backend) noexcept;
```

<a id="libtmux-capabilities-hpp-free-symbols-to-string-3"></a>
#### `to_string`

```cpp
[[nodiscard]] constexpr std::string_view to_string(ServerFeature feature) noexcept;
```

<a id="libtmux-entities-hpp"></a>
## `libtmux/entities.hpp`

The tmux object hierarchy.  A Session, Window, Pane or Client is one row of a snapshot: a shared pointer to the listing it came from plus the index of its row. That representation is what lets an entity be copied, stored in a container and returned from a function with nothing to keep alive alongside it, while still costing no per-row allocation and no tmux call to read.  Reading a field is local and cannot fail. Every method returning `expected` runs tmux, and a returned entity describes the moment that command ran: entities do not update themselves, `refresh` takes a new snapshot.  Psmux numbers windows and panes per session, so Windows snapshots retain their owning session identity alongside tmux's `$0`, `@0`, and `%0` IDs.  Every field below is a format token tmux 3.2a already registers, which is the oldest version this library supports. That is a hard constraint rather than a preference: tmux expands a token it does not know to the empty string, so requesting a newer one would read as a present-but-empty value on an older server instead of failing.

**Symbols:**

- [`SplitOptions`](#libtmux-entities-hpp-splitoptions)
  - [`SplitOptions::horizontal`](#libtmux-entities-hpp-splitoptions-horizontal)
  - [`SplitOptions::before`](#libtmux-entities-hpp-splitoptions-before)
  - [`SplitOptions::full_size`](#libtmux-entities-hpp-splitoptions-full-size)
  - [`SplitOptions::start_directory`](#libtmux-entities-hpp-splitoptions-start-directory)
  - [`SplitOptions::shell_command`](#libtmux-entities-hpp-splitoptions-shell-command)
  - [`SplitOptions::percentage`](#libtmux-entities-hpp-splitoptions-percentage)
  - [`SplitOptions::focus`](#libtmux-entities-hpp-splitoptions-focus)
  - [`SplitOptions::environment`](#libtmux-entities-hpp-splitoptions-environment)
- [`NewWindowOptions`](#libtmux-entities-hpp-newwindowoptions)
  - [`NewWindowOptions::name`](#libtmux-entities-hpp-newwindowoptions-name)
  - [`NewWindowOptions::start_directory`](#libtmux-entities-hpp-newwindowoptions-start-directory)
  - [`NewWindowOptions::shell_command`](#libtmux-entities-hpp-newwindowoptions-shell-command)
  - [`NewWindowOptions::after_current`](#libtmux-entities-hpp-newwindowoptions-after-current)
  - [`NewWindowOptions::index`](#libtmux-entities-hpp-newwindowoptions-index)
  - [`NewWindowOptions::focus`](#libtmux-entities-hpp-newwindowoptions-focus)
  - [`NewWindowOptions::environment`](#libtmux-entities-hpp-newwindowoptions-environment)
- [`NewSessionOptions`](#libtmux-entities-hpp-newsessionoptions)
  - [`NewSessionOptions::name`](#libtmux-entities-hpp-newsessionoptions-name)
  - [`NewSessionOptions::start_directory`](#libtmux-entities-hpp-newsessionoptions-start-directory)
  - [`NewSessionOptions::first_window_name`](#libtmux-entities-hpp-newsessionoptions-first-window-name)
  - [`NewSessionOptions::shell_command`](#libtmux-entities-hpp-newsessionoptions-shell-command)
  - [`NewSessionOptions::width`](#libtmux-entities-hpp-newsessionoptions-width)
  - [`NewSessionOptions::height`](#libtmux-entities-hpp-newsessionoptions-height)
  - [`NewSessionOptions::environment`](#libtmux-entities-hpp-newsessionoptions-environment)
- [`CaptureOptions`](#libtmux-entities-hpp-captureoptions)
  - [`CaptureOptions::start_line`](#libtmux-entities-hpp-captureoptions-start-line)
  - [`CaptureOptions::end_line`](#libtmux-entities-hpp-captureoptions-end-line)
  - [`CaptureOptions::whole_history`](#libtmux-entities-hpp-captureoptions-whole-history)
  - [`CaptureOptions::join_wrapped`](#libtmux-entities-hpp-captureoptions-join-wrapped)
  - [`CaptureOptions::with_escape_sequences`](#libtmux-entities-hpp-captureoptions-with-escape-sequences)
  - [`CaptureOptions::keep_trailing_spaces`](#libtmux-entities-hpp-captureoptions-keep-trailing-spaces)
  - [`CaptureOptions::output_limit`](#libtmux-entities-hpp-captureoptions-output-limit)
- [`Session`](#libtmux-entities-hpp-session)
  - [`Session::kNoun`](#libtmux-entities-hpp-session-knoun)
  - [`Session::kFields`](#libtmux-entities-hpp-session-kfields)
  - [`Session::Session`](#libtmux-entities-hpp-session-session)
  - [`Session::connection_identity`](#libtmux-entities-hpp-session-connection-identity)
  - [`Session::server`](#libtmux-entities-hpp-session-server)
  - [`Session::id`](#libtmux-entities-hpp-session-id)
  - [`Session::name`](#libtmux-entities-hpp-session-name)
  - [`Session::attached`](#libtmux-entities-hpp-session-attached)
  - [`Session::client_count`](#libtmux-entities-hpp-session-client-count)
  - [`Session::window_count`](#libtmux-entities-hpp-session-window-count)
  - [`Session::path`](#libtmux-entities-hpp-session-path)
  - [`Session::created`](#libtmux-entities-hpp-session-created)
  - [`Session::group`](#libtmux-entities-hpp-session-group)
  - [`Session::grouped`](#libtmux-entities-hpp-session-grouped)
  - [`Session::operator==`](#libtmux-entities-hpp-session-operator)
  - [`Session::windows`](#libtmux-entities-hpp-session-windows)
  - [`Session::panes`](#libtmux-entities-hpp-session-panes)
  - [`Session::active_window`](#libtmux-entities-hpp-session-active-window)
  - [`Session::active_pane`](#libtmux-entities-hpp-session-active-pane)
  - [`Session::select_next_window`](#libtmux-entities-hpp-session-select-next-window)
  - [`Session::select_previous_window`](#libtmux-entities-hpp-session-select-previous-window)
  - [`Session::select_last_window`](#libtmux-entities-hpp-session-select-last-window)
  - [`Session::new_window`](#libtmux-entities-hpp-session-new-window)
  - [`Session::new_window`](#libtmux-entities-hpp-session-new-window-2)
  - [`Session::rename`](#libtmux-entities-hpp-session-rename)
  - [`Session::kill`](#libtmux-entities-hpp-session-kill)
  - [`Session::refresh`](#libtmux-entities-hpp-session-refresh)
  - [`Session::options`](#libtmux-entities-hpp-session-options)
  - [`Session::option`](#libtmux-entities-hpp-session-option)
  - [`Session::set_option`](#libtmux-entities-hpp-session-set-option)
  - [`Session::unset_option`](#libtmux-entities-hpp-session-unset-option)
  - [`Session::attach_command`](#libtmux-entities-hpp-session-attach-command)
  - [`Session::checked_attach_command`](#libtmux-entities-hpp-session-checked-attach-command)
  - [`Session::detach_clients`](#libtmux-entities-hpp-session-detach-clients)
  - [`Session::expand`](#libtmux-entities-hpp-session-expand)
  - [`Session::show_message`](#libtmux-entities-hpp-session-show-message)
  - [`Session::hooks`](#libtmux-entities-hpp-session-hooks)
  - [`Session::set_hook`](#libtmux-entities-hpp-session-set-hook)
- [`Window`](#libtmux-entities-hpp-window)
  - [`Window::kNoun`](#libtmux-entities-hpp-window-knoun)
  - [`Window::kSessionNameField`](#libtmux-entities-hpp-window-ksessionnamefield)
  - [`Window::kFields`](#libtmux-entities-hpp-window-kfields)
  - [`Window::Window`](#libtmux-entities-hpp-window-window)
  - [`Window::connection_identity`](#libtmux-entities-hpp-window-connection-identity)
  - [`Window::server`](#libtmux-entities-hpp-window-server)
  - [`Window::id`](#libtmux-entities-hpp-window-id)
  - [`Window::name`](#libtmux-entities-hpp-window-name)
  - [`Window::active`](#libtmux-entities-hpp-window-active)
  - [`Window::session_id`](#libtmux-entities-hpp-window-session-id)
  - [`Window::index`](#libtmux-entities-hpp-window-index)
  - [`Window::pane_count`](#libtmux-entities-hpp-window-pane-count)
  - [`Window::width`](#libtmux-entities-hpp-window-width)
  - [`Window::height`](#libtmux-entities-hpp-window-height)
  - [`Window::layout`](#libtmux-entities-hpp-window-layout)
  - [`Window::zoomed`](#libtmux-entities-hpp-window-zoomed)
  - [`Window::bell`](#libtmux-entities-hpp-window-bell)
  - [`Window::activity`](#libtmux-entities-hpp-window-activity)
  - [`Window::linked_sessions`](#libtmux-entities-hpp-window-linked-sessions)
  - [`Window::session_name`](#libtmux-entities-hpp-window-session-name)
  - [`Window::operator==`](#libtmux-entities-hpp-window-operator)
  - [`Window::target`](#libtmux-entities-hpp-window-target)
  - [`Window::checked_target`](#libtmux-entities-hpp-window-checked-target)
  - [`Window::session`](#libtmux-entities-hpp-window-session)
  - [`Window::panes`](#libtmux-entities-hpp-window-panes)
  - [`Window::active_pane`](#libtmux-entities-hpp-window-active-pane)
  - [`Window::split`](#libtmux-entities-hpp-window-split)
  - [`Window::split`](#libtmux-entities-hpp-window-split-2)
  - [`Window::rename`](#libtmux-entities-hpp-window-rename)
  - [`Window::select_layout`](#libtmux-entities-hpp-window-select-layout)
  - [`Window::resize`](#libtmux-entities-hpp-window-resize)
  - [`Window::next_layout`](#libtmux-entities-hpp-window-next-layout)
  - [`Window::previous_layout`](#libtmux-entities-hpp-window-previous-layout)
  - [`Window::rotate`](#libtmux-entities-hpp-window-rotate)
  - [`Window::select_last_pane`](#libtmux-entities-hpp-window-select-last-pane)
  - [`Window::link_to`](#libtmux-entities-hpp-window-link-to)
  - [`Window::unlink`](#libtmux-entities-hpp-window-unlink)
  - [`Window::swap_with`](#libtmux-entities-hpp-window-swap-with)
  - [`Window::move_to`](#libtmux-entities-hpp-window-move-to)
  - [`Window::expand`](#libtmux-entities-hpp-window-expand)
  - [`Window::show_message`](#libtmux-entities-hpp-window-show-message)
  - [`Window::select`](#libtmux-entities-hpp-window-select)
  - [`Window::kill`](#libtmux-entities-hpp-window-kill)
  - [`Window::refresh`](#libtmux-entities-hpp-window-refresh)
  - [`Window::options`](#libtmux-entities-hpp-window-options)
  - [`Window::option`](#libtmux-entities-hpp-window-option)
  - [`Window::set_option`](#libtmux-entities-hpp-window-set-option)
  - [`Window::unset_option`](#libtmux-entities-hpp-window-unset-option)
- [`Pane`](#libtmux-entities-hpp-pane)
  - [`Pane::kNoun`](#libtmux-entities-hpp-pane-knoun)
  - [`Pane::kSessionNameField`](#libtmux-entities-hpp-pane-ksessionnamefield)
  - [`Pane::kFields`](#libtmux-entities-hpp-pane-kfields)
  - [`Pane::Pane`](#libtmux-entities-hpp-pane-pane)
  - [`Pane::connection_identity`](#libtmux-entities-hpp-pane-connection-identity)
  - [`Pane::server`](#libtmux-entities-hpp-pane-server)
  - [`Pane::id`](#libtmux-entities-hpp-pane-id)
  - [`Pane::command`](#libtmux-entities-hpp-pane-command)
  - [`Pane::active`](#libtmux-entities-hpp-pane-active)
  - [`Pane::window_id`](#libtmux-entities-hpp-pane-window-id)
  - [`Pane::session_id`](#libtmux-entities-hpp-pane-session-id)
  - [`Pane::index`](#libtmux-entities-hpp-pane-index)
  - [`Pane::title`](#libtmux-entities-hpp-pane-title)
  - [`Pane::pid`](#libtmux-entities-hpp-pane-pid)
  - [`Pane::tty`](#libtmux-entities-hpp-pane-tty)
  - [`Pane::path`](#libtmux-entities-hpp-pane-path)
  - [`Pane::width`](#libtmux-entities-hpp-pane-width)
  - [`Pane::height`](#libtmux-entities-hpp-pane-height)
  - [`Pane::dead`](#libtmux-entities-hpp-pane-dead)
  - [`Pane::in_mode`](#libtmux-entities-hpp-pane-in-mode)
  - [`Pane::at_top`](#libtmux-entities-hpp-pane-at-top)
  - [`Pane::at_bottom`](#libtmux-entities-hpp-pane-at-bottom)
  - [`Pane::at_left`](#libtmux-entities-hpp-pane-at-left)
  - [`Pane::at_right`](#libtmux-entities-hpp-pane-at-right)
  - [`Pane::piping`](#libtmux-entities-hpp-pane-piping)
  - [`Pane::session_name`](#libtmux-entities-hpp-pane-session-name)
  - [`Pane::operator==`](#libtmux-entities-hpp-pane-operator)
  - [`Pane::window`](#libtmux-entities-hpp-pane-window)
  - [`Pane::session`](#libtmux-entities-hpp-pane-session)
  - [`Pane::send_text`](#libtmux-entities-hpp-pane-send-text)
  - [`Pane::send_key`](#libtmux-entities-hpp-pane-send-key)
  - [`Pane::capture`](#libtmux-entities-hpp-pane-capture)
  - [`Pane::capture`](#libtmux-entities-hpp-pane-capture-2)
  - [`Pane::set_width`](#libtmux-entities-hpp-pane-set-width)
  - [`Pane::set_height`](#libtmux-entities-hpp-pane-set-height)
  - [`Pane::swap_with`](#libtmux-entities-hpp-pane-swap-with)
  - [`Pane::break_out`](#libtmux-entities-hpp-pane-break-out)
  - [`Pane::join`](#libtmux-entities-hpp-pane-join)
  - [`Pane::enter_copy_mode`](#libtmux-entities-hpp-pane-enter-copy-mode)
  - [`Pane::leave_mode`](#libtmux-entities-hpp-pane-leave-mode)
  - [`Pane::pipe_to`](#libtmux-entities-hpp-pane-pipe-to)
  - [`Pane::stop_piping`](#libtmux-entities-hpp-pane-stop-piping)
  - [`Pane::set_title`](#libtmux-entities-hpp-pane-set-title)
  - [`Pane::respawn`](#libtmux-entities-hpp-pane-respawn)
  - [`Pane::clear_history`](#libtmux-entities-hpp-pane-clear-history)
  - [`Pane::expand`](#libtmux-entities-hpp-pane-expand)
  - [`Pane::show_message`](#libtmux-entities-hpp-pane-show-message)
  - [`Pane::paste`](#libtmux-entities-hpp-pane-paste)
  - [`Pane::select`](#libtmux-entities-hpp-pane-select)
  - [`Pane::kill`](#libtmux-entities-hpp-pane-kill)
  - [`Pane::refresh`](#libtmux-entities-hpp-pane-refresh)
  - [`Pane::options`](#libtmux-entities-hpp-pane-options)
  - [`Pane::option`](#libtmux-entities-hpp-pane-option)
  - [`Pane::set_option`](#libtmux-entities-hpp-pane-set-option)
  - [`Pane::unset_option`](#libtmux-entities-hpp-pane-unset-option)
- [`Command`](#libtmux-entities-hpp-command)
  - [`Command::kNoun`](#libtmux-entities-hpp-command-knoun)
  - [`Command::kFields`](#libtmux-entities-hpp-command-kfields)
  - [`Command::Command`](#libtmux-entities-hpp-command-command)
  - [`Command::connection_identity`](#libtmux-entities-hpp-command-connection-identity)
  - [`Command::server`](#libtmux-entities-hpp-command-server)
  - [`Command::name`](#libtmux-entities-hpp-command-name)
  - [`Command::alias`](#libtmux-entities-hpp-command-alias)
  - [`Command::usage`](#libtmux-entities-hpp-command-usage)
  - [`Command::operator==`](#libtmux-entities-hpp-command-operator)
- [`Buffer`](#libtmux-entities-hpp-buffer)
  - [`Buffer::kNoun`](#libtmux-entities-hpp-buffer-knoun)
  - [`Buffer::kFields`](#libtmux-entities-hpp-buffer-kfields)
  - [`Buffer::Buffer`](#libtmux-entities-hpp-buffer-buffer)
  - [`Buffer::connection_identity`](#libtmux-entities-hpp-buffer-connection-identity)
  - [`Buffer::server`](#libtmux-entities-hpp-buffer-server)
  - [`Buffer::name`](#libtmux-entities-hpp-buffer-name)
  - [`Buffer::size`](#libtmux-entities-hpp-buffer-size)
  - [`Buffer::sample`](#libtmux-entities-hpp-buffer-sample)
  - [`Buffer::created`](#libtmux-entities-hpp-buffer-created)
  - [`Buffer::operator==`](#libtmux-entities-hpp-buffer-operator)
  - [`Buffer::contents`](#libtmux-entities-hpp-buffer-contents)
  - [`Buffer::remove`](#libtmux-entities-hpp-buffer-remove)
- [`Client`](#libtmux-entities-hpp-client)
  - [`Client::kNoun`](#libtmux-entities-hpp-client-knoun)
  - [`Client::kFields`](#libtmux-entities-hpp-client-kfields)
  - [`Client::Client`](#libtmux-entities-hpp-client-client)
  - [`Client::connection_identity`](#libtmux-entities-hpp-client-connection-identity)
  - [`Client::server`](#libtmux-entities-hpp-client-server)
  - [`Client::name`](#libtmux-entities-hpp-client-name)
  - [`Client::session_name`](#libtmux-entities-hpp-client-session-name)
  - [`Client::read_only`](#libtmux-entities-hpp-client-read-only)
  - [`Client::tty`](#libtmux-entities-hpp-client-tty)
  - [`Client::width`](#libtmux-entities-hpp-client-width)
  - [`Client::height`](#libtmux-entities-hpp-client-height)
  - [`Client::created`](#libtmux-entities-hpp-client-created)
  - [`Client::last_activity`](#libtmux-entities-hpp-client-last-activity)
  - [`Client::terminal`](#libtmux-entities-hpp-client-terminal)
  - [`Client::control_mode`](#libtmux-entities-hpp-client-control-mode)
  - [`Client::operator==`](#libtmux-entities-hpp-client-operator)
  - [`Client::session`](#libtmux-entities-hpp-client-session)
  - [`Client::switch_to`](#libtmux-entities-hpp-client-switch-to)
  - [`Client::detach`](#libtmux-entities-hpp-client-detach)
  - [`Client::refresh`](#libtmux-entities-hpp-client-refresh)
- [`std::hash<libtmux::Session>`](#libtmux-entities-hpp-std-hash-libtmux-session)
  - [`std::hash<libtmux::Session>::operator()`](#libtmux-entities-hpp-std-hash-libtmux-session-operator)
- [`std::hash<libtmux::Window>`](#libtmux-entities-hpp-std-hash-libtmux-window)
  - [`std::hash<libtmux::Window>::operator()`](#libtmux-entities-hpp-std-hash-libtmux-window-operator)
- [`std::hash<libtmux::Pane>`](#libtmux-entities-hpp-std-hash-libtmux-pane)
  - [`std::hash<libtmux::Pane>::operator()`](#libtmux-entities-hpp-std-hash-libtmux-pane-operator)
- [`std::hash<libtmux::Client>`](#libtmux-entities-hpp-std-hash-libtmux-client)
  - [`std::hash<libtmux::Client>::operator()`](#libtmux-entities-hpp-std-hash-libtmux-client-operator)
- [`Free symbols`](#libtmux-entities-hpp-free-symbols)
  - [`operator<<`](#libtmux-entities-hpp-free-symbols-operator)
  - [`operator<<`](#libtmux-entities-hpp-free-symbols-operator-2)
  - [`operator<<`](#libtmux-entities-hpp-free-symbols-operator-3)
  - [`operator<<`](#libtmux-entities-hpp-free-symbols-operator-4)
  - [`session::id`](#libtmux-entities-hpp-free-symbols-session-id)
  - [`session::name`](#libtmux-entities-hpp-free-symbols-session-name)
  - [`session::attached`](#libtmux-entities-hpp-free-symbols-session-attached)
  - [`session::path`](#libtmux-entities-hpp-free-symbols-session-path)
  - [`session::group`](#libtmux-entities-hpp-free-symbols-session-group)
  - [`session::grouped`](#libtmux-entities-hpp-free-symbols-session-grouped)
  - [`session::client_count`](#libtmux-entities-hpp-free-symbols-session-client-count)
  - [`session::window_count`](#libtmux-entities-hpp-free-symbols-session-window-count)
  - [`window::id`](#libtmux-entities-hpp-free-symbols-window-id)
  - [`window::name`](#libtmux-entities-hpp-free-symbols-window-name)
  - [`window::active`](#libtmux-entities-hpp-free-symbols-window-active)
  - [`window::session_id`](#libtmux-entities-hpp-free-symbols-window-session-id)
  - [`window::session_name`](#libtmux-entities-hpp-free-symbols-window-session-name)
  - [`window::layout`](#libtmux-entities-hpp-free-symbols-window-layout)
  - [`window::zoomed`](#libtmux-entities-hpp-free-symbols-window-zoomed)
  - [`window::bell`](#libtmux-entities-hpp-free-symbols-window-bell)
  - [`window::activity`](#libtmux-entities-hpp-free-symbols-window-activity)
  - [`window::index`](#libtmux-entities-hpp-free-symbols-window-index)
  - [`window::pane_count`](#libtmux-entities-hpp-free-symbols-window-pane-count)
  - [`window::width`](#libtmux-entities-hpp-free-symbols-window-width)
  - [`window::height`](#libtmux-entities-hpp-free-symbols-window-height)
  - [`pane::id`](#libtmux-entities-hpp-free-symbols-pane-id)
  - [`pane::command`](#libtmux-entities-hpp-free-symbols-pane-command)
  - [`pane::active`](#libtmux-entities-hpp-free-symbols-pane-active)
  - [`pane::window_id`](#libtmux-entities-hpp-free-symbols-pane-window-id)
  - [`pane::session_id`](#libtmux-entities-hpp-free-symbols-pane-session-id)
  - [`pane::session_name`](#libtmux-entities-hpp-free-symbols-pane-session-name)
  - [`pane::title`](#libtmux-entities-hpp-free-symbols-pane-title)
  - [`pane::tty`](#libtmux-entities-hpp-free-symbols-pane-tty)
  - [`pane::path`](#libtmux-entities-hpp-free-symbols-pane-path)
  - [`pane::dead`](#libtmux-entities-hpp-free-symbols-pane-dead)
  - [`pane::in_mode`](#libtmux-entities-hpp-free-symbols-pane-in-mode)
  - [`pane::index`](#libtmux-entities-hpp-free-symbols-pane-index)
  - [`pane::pid`](#libtmux-entities-hpp-free-symbols-pane-pid)
  - [`pane::width`](#libtmux-entities-hpp-free-symbols-pane-width)
  - [`pane::height`](#libtmux-entities-hpp-free-symbols-pane-height)
  - [`client::name`](#libtmux-entities-hpp-free-symbols-client-name)
  - [`client::session_name`](#libtmux-entities-hpp-free-symbols-client-session-name)
  - [`client::read_only`](#libtmux-entities-hpp-free-symbols-client-read-only)
  - [`client::tty`](#libtmux-entities-hpp-free-symbols-client-tty)
  - [`client::terminal`](#libtmux-entities-hpp-free-symbols-client-terminal)
  - [`client::control_mode`](#libtmux-entities-hpp-free-symbols-client-control-mode)
  - [`client::width`](#libtmux-entities-hpp-free-symbols-client-width)
  - [`client::height`](#libtmux-entities-hpp-free-symbols-client-height)

<a id="libtmux-entities-hpp-splitoptions"></a>
### `SplitOptions`

```cpp
struct SplitOptions;
```

<a id="libtmux-entities-hpp-splitoptions-horizontal"></a>
#### `SplitOptions::horizontal`

```cpp
bool horizontal{false};
```
Side by side. tmux stacks by default.

<a id="libtmux-entities-hpp-splitoptions-before"></a>
#### `SplitOptions::before`

```cpp
bool before{false};
```
Before the target rather than after it.

<a id="libtmux-entities-hpp-splitoptions-full-size"></a>
#### `SplitOptions::full_size`

```cpp
bool full_size{false};
```
Spanning the full width or height of the window rather than of the pane.

<a id="libtmux-entities-hpp-splitoptions-start-directory"></a>
#### `SplitOptions::start_directory`

```cpp
std::string start_directory{};
```
Where the new pane starts. Empty inherits from the window.

<a id="libtmux-entities-hpp-splitoptions-shell-command"></a>
#### `SplitOptions::shell_command`

```cpp
std::string shell_command{};
```
Run this instead of the default shell.

<a id="libtmux-entities-hpp-splitoptions-percentage"></a>
#### `SplitOptions::percentage`

```cpp
std::optional<int> percentage{};
```
Size as a percentage of the space being divided.

<a id="libtmux-entities-hpp-splitoptions-focus"></a>
#### `SplitOptions::focus`

```cpp
bool focus{false};
```
Make the new pane the active one.

<a id="libtmux-entities-hpp-splitoptions-environment"></a>
#### `SplitOptions::environment`

```cpp
std::vector<std::pair<std::string, std::string>> environment{};
```
Variables the new process starts with, on top of what tmux passes down.  Pairs rather than `NAME=value` strings: tmux takes a `-e` without an `=` without complaint and creates nothing, so the pair is joined here and a name carrying an `=` is refused where the command is built.  An empty value sets the variable to empty. It does not remove it.

<a id="libtmux-entities-hpp-newwindowoptions"></a>
### `NewWindowOptions`

```cpp
struct NewWindowOptions;
```

<a id="libtmux-entities-hpp-newwindowoptions-name"></a>
#### `NewWindowOptions::name`

```cpp
std::string name{};
```

<a id="libtmux-entities-hpp-newwindowoptions-start-directory"></a>
#### `NewWindowOptions::start_directory`

```cpp
std::string start_directory{};
```

<a id="libtmux-entities-hpp-newwindowoptions-shell-command"></a>
#### `NewWindowOptions::shell_command`

```cpp
std::string shell_command{};
```

<a id="libtmux-entities-hpp-newwindowoptions-after-current"></a>
#### `NewWindowOptions::after_current`

```cpp
bool after_current{false};
```
Immediately after the current window rather than at the end.

<a id="libtmux-entities-hpp-newwindowoptions-index"></a>
#### `NewWindowOptions::index`

```cpp
std::optional<long long> index{};
```
Put the window at this index rather than at the next free one.  tmux refuses an index already in use rather than shifting anything, and that refusal is kept: a workspace rebuilt over a running one should say so rather than quietly land somewhere else.

<a id="libtmux-entities-hpp-newwindowoptions-focus"></a>
#### `NewWindowOptions::focus`

```cpp
bool focus{false};
```

<a id="libtmux-entities-hpp-newwindowoptions-environment"></a>
#### `NewWindowOptions::environment`

```cpp
std::vector<std::pair<std::string, std::string>> environment{};
```
Variables the new process starts with, on top of what tmux passes down.  Pairs rather than `NAME=value` strings: tmux takes a `-e` without an `=` without complaint and creates nothing, so the pair is joined here and a name carrying an `=` is refused where the command is built.  An empty value sets the variable to empty. It does not remove it.

<a id="libtmux-entities-hpp-newsessionoptions"></a>
### `NewSessionOptions`

```cpp
struct NewSessionOptions;
```

<a id="libtmux-entities-hpp-newsessionoptions-name"></a>
#### `NewSessionOptions::name`

```cpp
std::string name{};
```

<a id="libtmux-entities-hpp-newsessionoptions-start-directory"></a>
#### `NewSessionOptions::start_directory`

```cpp
std::string start_directory{};
```

<a id="libtmux-entities-hpp-newsessionoptions-first-window-name"></a>
#### `NewSessionOptions::first_window_name`

```cpp
std::string first_window_name{};
```
The name of the window the session starts with.

<a id="libtmux-entities-hpp-newsessionoptions-shell-command"></a>
#### `NewSessionOptions::shell_command`

```cpp
std::string shell_command{};
```

<a id="libtmux-entities-hpp-newsessionoptions-width"></a>
#### `NewSessionOptions::width`

```cpp
std::optional<int> width{};
```
The size tmux gives the session while no client is attached. Without it a detached session is 80x24, and a pane that reports its size to a program reports that.

<a id="libtmux-entities-hpp-newsessionoptions-height"></a>
#### `NewSessionOptions::height`

```cpp
std::optional<int> height{};
```

<a id="libtmux-entities-hpp-newsessionoptions-environment"></a>
#### `NewSessionOptions::environment`

```cpp
std::vector<std::pair<std::string, std::string>> environment{};
```
Variables the new process starts with, on top of what tmux passes down.  Pairs rather than `NAME=value` strings: tmux takes a `-e` without an `=` without complaint and creates nothing, so the pair is joined here and a name carrying an `=` is refused where the command is built.  An empty value sets the variable to empty. It does not remove it.

<a id="libtmux-entities-hpp-captureoptions"></a>
### `CaptureOptions`

```cpp
struct CaptureOptions;
```

<a id="libtmux-entities-hpp-captureoptions-start-line"></a>
#### `CaptureOptions::start_line`

```cpp
std::optional<int> start_line{};
```
Where to start, counting back into the scrollback. Absent starts at the top of the visible pane.

<a id="libtmux-entities-hpp-captureoptions-end-line"></a>
#### `CaptureOptions::end_line`

```cpp
std::optional<int> end_line{};
```

<a id="libtmux-entities-hpp-captureoptions-whole-history"></a>
#### `CaptureOptions::whole_history`

```cpp
bool whole_history{false};
```
Everything tmux still remembers, which is what a caller reading history usually means.

<a id="libtmux-entities-hpp-captureoptions-join-wrapped"></a>
#### `CaptureOptions::join_wrapped`

```cpp
bool join_wrapped{false};
```
Join a line tmux wrapped back into the one line it was.

<a id="libtmux-entities-hpp-captureoptions-with-escape-sequences"></a>
#### `CaptureOptions::with_escape_sequences`

```cpp
bool with_escape_sequences{false};
```
Keep the escape sequences rather than the text they produced.

<a id="libtmux-entities-hpp-captureoptions-keep-trailing-spaces"></a>
#### `CaptureOptions::keep_trailing_spaces`

```cpp
bool keep_trailing_spaces{false};
```
Keep the spaces at the end of a line, which tmux otherwise trims.

<a id="libtmux-entities-hpp-captureoptions-output-limit"></a>
#### `CaptureOptions::output_limit`

```cpp
std::optional<std::size_t> output_limit{};
```
How much of the answer this call is prepared to hold. A scrollback can be far larger than the default, and one that does not fit is reported.

<a id="libtmux-entities-hpp-session"></a>
### `Session`

```cpp
class Session;
```

<a id="libtmux-entities-hpp-session-knoun"></a>
#### `Session::kNoun`

```cpp
static constexpr std::string_view kNoun{"session"};
```

<a id="libtmux-entities-hpp-session-kfields"></a>
#### `Session::kFields`

```cpp
static constexpr std::array kFields{ std::string_view{"session_id"}, std::string_view{"session_name"}, std::string_view{"session_attached"}, std::string_view{"session_windows"}, std::string_view{"session_path"}, std::string_view{"session_created"}, std::string_view{"session_group"}, std::string_view{"session_grouped"}};
```

<a id="libtmux-entities-hpp-session-session"></a>
#### `Session::Session`

```cpp
Session(std::shared_ptr<const Snapshot> snapshot, std::size_t row) noexcept;
```

<a id="libtmux-entities-hpp-session-connection-identity"></a>
#### `Session::connection_identity`

```cpp
using Row::connection_identity;
```

<a id="libtmux-entities-hpp-session-server"></a>
#### `Session::server`

```cpp
using Row::server;
```

<a id="libtmux-entities-hpp-session-id"></a>
#### `Session::id`

```cpp
[[nodiscard]] std::string_view id() const noexcept;
```

<a id="libtmux-entities-hpp-session-name"></a>
#### `Session::name`

```cpp
[[nodiscard]] std::string_view name() const noexcept;
```

<a id="libtmux-entities-hpp-session-attached"></a>
#### `Session::attached`

```cpp
[[nodiscard]] bool attached() const noexcept;
```
`session_attached` counts clients rather than rendering a flag, so any count other than zero means attached.

<a id="libtmux-entities-hpp-session-client-count"></a>
#### `Session::client_count`

```cpp
[[nodiscard]] long long client_count() const noexcept;
```

<a id="libtmux-entities-hpp-session-window-count"></a>
#### `Session::window_count`

```cpp
[[nodiscard]] long long window_count() const noexcept;
```

<a id="libtmux-entities-hpp-session-path"></a>
#### `Session::path`

```cpp
[[nodiscard]] std::string_view path() const noexcept;
```
The directory a new window starts in, not the shell's current directory.

<a id="libtmux-entities-hpp-session-created"></a>
#### `Session::created`

```cpp
[[nodiscard]] std::chrono::sys_seconds created() const noexcept;
```

<a id="libtmux-entities-hpp-session-group"></a>
#### `Session::group`

```cpp
[[nodiscard]] std::string_view group() const noexcept;
```
Empty unless the session belongs to a group sharing its windows.

<a id="libtmux-entities-hpp-session-grouped"></a>
#### `Session::grouped`

```cpp
[[nodiscard]] bool grouped() const noexcept;
```

<a id="libtmux-entities-hpp-session-operator"></a>
#### `Session::operator==`

```cpp
[[nodiscard]] bool operator==(const Session& other) const noexcept;
```
Two values are the same session when they name the same tmux object on the same connection — not when they were listed at the same moment. A session refreshed after a rename equals the one it was refreshed from.

<a id="libtmux-entities-hpp-session-windows"></a>
#### `Session::windows`

```cpp
[[nodiscard]] expected<std::vector<Window>, CommandFailure> windows() const;
```

<a id="libtmux-entities-hpp-session-panes"></a>
#### `Session::panes`

```cpp
[[nodiscard]] expected<std::vector<Pane>, CommandFailure> panes() const;
```

<a id="libtmux-entities-hpp-session-active-window"></a>
#### `Session::active_window`

```cpp
[[nodiscard]] expected<Window, CommandFailure> active_window() const;
```

<a id="libtmux-entities-hpp-session-active-pane"></a>
#### `Session::active_pane`

```cpp
[[nodiscard]] expected<Pane, CommandFailure> active_pane() const;
```

<a id="libtmux-entities-hpp-session-select-next-window"></a>
#### `Session::select_next_window`

```cpp
[[nodiscard]] expected<Window, CommandFailure> select_next_window() const;
```
Move the selection, and answer with the window it landed on.  Named for what they do rather than for what they return: `next_window()` would read as a question, and these change which window is active.  Relative navigation is tmux's to perform, not a caller's to compute. Next and previous wrap around the window list, and "last" means the previously selected window — state only the server holds, which a caller listing windows has no way to reconstruct.  Each fails when there is nowhere to go, as tmux does: a session with one window refuses all three rather than selecting the window already active.

<a id="libtmux-entities-hpp-session-select-previous-window"></a>
#### `Session::select_previous_window`

```cpp
[[nodiscard]] expected<Window, CommandFailure> select_previous_window() const;
```

<a id="libtmux-entities-hpp-session-select-last-window"></a>
#### `Session::select_last_window`

```cpp
[[nodiscard]] expected<Window, CommandFailure> select_last_window() const;
```

<a id="libtmux-entities-hpp-session-new-window"></a>
#### `Session::new_window`

```cpp
[[nodiscard]] expected<Window, CommandFailure> new_window(std::string_view name) const;
```
Created detached: a library call that stole the terminal would be a surprise, and attaching is a separate decision.

<a id="libtmux-entities-hpp-session-new-window-2"></a>
#### `Session::new_window`

```cpp
[[nodiscard]] expected<Window, CommandFailure> new_window(NewWindowOptions options) const;
```

<a id="libtmux-entities-hpp-session-rename"></a>
#### `Session::rename`

```cpp
[[nodiscard]] expected<void, CommandFailure> rename(std::string_view name) const;
```

<a id="libtmux-entities-hpp-session-kill"></a>
#### `Session::kill`

```cpp
[[nodiscard]] expected<void, CommandFailure> kill() const;
```

<a id="libtmux-entities-hpp-session-refresh"></a>
#### `Session::refresh`

```cpp
[[nodiscard]] expected<Session, CommandFailure> refresh() const;
```

<a id="libtmux-entities-hpp-session-options"></a>
#### `Session::options`

```cpp
[[nodiscard]] expected<std::vector<OptionEntry>, CommandFailure> options() const;
```
Session options. Reading reports the value tmux would use, marking one that comes from a wider scope as inherited rather than hiding it.

<a id="libtmux-entities-hpp-session-option"></a>
#### `Session::option`

```cpp
[[nodiscard]] expected<OptionEntry, CommandFailure> option(std::string_view name) const;
```

<a id="libtmux-entities-hpp-session-set-option"></a>
#### `Session::set_option`

```cpp
[[nodiscard]] expected<void, CommandFailure> set_option(std::string_view name, std::string_view value) const;
```

<a id="libtmux-entities-hpp-session-unset-option"></a>
#### `Session::unset_option`

```cpp
[[nodiscard]] expected<void, CommandFailure> unset_option(std::string_view name) const;
```
Remove the value set here, so the wider scope shows through again.

<a id="libtmux-entities-hpp-session-attach-command"></a>
#### `Session::attach_command`

```cpp
[[nodiscard]] std::vector<std::string> attach_command() const;
```
The command line that attaches a terminal to this session.  Not a method that attaches: a tmux client needs a terminal, and every command this library runs talks to it through pipes, so an attach it performed itself could only ever fail. A caller that owns a terminal execs this instead.  Prefer `checked_attach_command()`; this source-compatible form returns an empty vector when psmux cannot bind an attach target without a stale race.

<a id="libtmux-entities-hpp-session-checked-attach-command"></a>
#### `Session::checked_attach_command`

```cpp
[[nodiscard]] expected<std::vector<std::string>, CommandFailure> checked_attach_command() const;
```

<a id="libtmux-entities-hpp-session-detach-clients"></a>
#### `Session::detach_clients`

```cpp
[[nodiscard]] expected<void, CommandFailure> detach_clients() const;
```
Send every client here away, leaving the session running.

<a id="libtmux-entities-hpp-session-expand"></a>
#### `Session::expand`

```cpp
[[nodiscard]] expected<std::string, CommandFailure> expand(std::string_view format) const;
```
Ask tmux to expand a format against this session. The fields above are what a session is; this reaches the rest of tmux's vocabulary without a method per variable.  A target tmux cannot find is not an error to tmux. It expands the fields it cannot resolve to nothing, prints the literals around them, and exits zero — so a session that has been killed answers with a blank that reads like a value. This asks for the session's own id alongside the caller's format and reports `missing` when the answer is not this session.

<a id="libtmux-entities-hpp-session-show-message"></a>
#### `Session::show_message`

```cpp
[[nodiscard]] expected<void, CommandFailure> show_message(std::string_view text) const;
```
Put a message on the status line of every client attached here, and send it to a control client as `%message`.  tmux expands the text as a format, so a `#{...}` in it is substituted rather than shown. Text built from data belongs in `escape_literal` first.

<a id="libtmux-entities-hpp-session-hooks"></a>
#### `Session::hooks`

```cpp
[[nodiscard]] expected<std::vector<OptionEntry>, CommandFailure> hooks() const;
```

<a id="libtmux-entities-hpp-session-set-hook"></a>
#### `Session::set_hook`

```cpp
[[nodiscard]] expected<void, CommandFailure> set_hook(std::string_view name, std::string_view command) const;
```

<a id="libtmux-entities-hpp-window"></a>
### `Window`

```cpp
class Window;
```

<a id="libtmux-entities-hpp-window-knoun"></a>
#### `Window::kNoun`

```cpp
static constexpr std::string_view kNoun{"window"};
```

<a id="libtmux-entities-hpp-window-ksessionnamefield"></a>
#### `Window::kSessionNameField`

```cpp
static constexpr std::string_view kSessionNameField{"session_name"};
```

<a id="libtmux-entities-hpp-window-kfields"></a>
#### `Window::kFields`

```cpp
static constexpr std::array kFields{std::string_view{"window_id"}, std::string_view{"window_name"}, std::string_view{"window_active"}, std::string_view{"session_id"}, std::string_view{"window_index"}, std::string_view{"window_panes"}, std::string_view{"window_width"}, std::string_view{"window_height"}, std::string_view{"window_layout"}, std::string_view{"window_zoomed_flag"}, std::string_view{"window_bell_flag"}, std::string_view{"window_activity_flag"}, std::string_view{"window_linked_sessions"}};
```

<a id="libtmux-entities-hpp-window-window"></a>
#### `Window::Window`

```cpp
Window(std::shared_ptr<const Snapshot> snapshot, std::size_t row) noexcept;
```

<a id="libtmux-entities-hpp-window-connection-identity"></a>
#### `Window::connection_identity`

```cpp
using Row::connection_identity;
```

<a id="libtmux-entities-hpp-window-server"></a>
#### `Window::server`

```cpp
using Row::server;
```

<a id="libtmux-entities-hpp-window-id"></a>
#### `Window::id`

```cpp
[[nodiscard]] std::string_view id() const noexcept;
```

<a id="libtmux-entities-hpp-window-name"></a>
#### `Window::name`

```cpp
[[nodiscard]] std::string_view name() const noexcept;
```

<a id="libtmux-entities-hpp-window-active"></a>
#### `Window::active`

```cpp
[[nodiscard]] bool active() const noexcept;
```

<a id="libtmux-entities-hpp-window-session-id"></a>
#### `Window::session_id`

```cpp
[[nodiscard]] std::string_view session_id() const noexcept;
```
The link to the parent, carried in the row so traversal upward costs nothing until the parent itself is wanted.

<a id="libtmux-entities-hpp-window-index"></a>
#### `Window::index`

```cpp
[[nodiscard]] long long index() const noexcept;
```
Position within its session, which `base-index` is free to start anywhere.

<a id="libtmux-entities-hpp-window-pane-count"></a>
#### `Window::pane_count`

```cpp
[[nodiscard]] long long pane_count() const noexcept;
```

<a id="libtmux-entities-hpp-window-width"></a>
#### `Window::width`

```cpp
[[nodiscard]] long long width() const noexcept;
```

<a id="libtmux-entities-hpp-window-height"></a>
#### `Window::height`

```cpp
[[nodiscard]] long long height() const noexcept;
```

<a id="libtmux-entities-hpp-window-layout"></a>
#### `Window::layout`

```cpp
[[nodiscard]] std::string_view layout() const noexcept;
```
tmux's own layout description, which `select-layout` accepts back.

<a id="libtmux-entities-hpp-window-zoomed"></a>
#### `Window::zoomed`

```cpp
[[nodiscard]] bool zoomed() const noexcept;
```

<a id="libtmux-entities-hpp-window-bell"></a>
#### `Window::bell`

```cpp
[[nodiscard]] bool bell() const noexcept;
```

<a id="libtmux-entities-hpp-window-activity"></a>
#### `Window::activity`

```cpp
[[nodiscard]] bool activity() const noexcept;
```

<a id="libtmux-entities-hpp-window-linked-sessions"></a>
#### `Window::linked_sessions`

```cpp
[[nodiscard]] long long linked_sessions() const noexcept;
```
How many sessions hold this window. More than one means the same window is shown in several places, and a command aimed at a bare id could land on any of them — which is why targets here are qualified.

<a id="libtmux-entities-hpp-window-session-name"></a>
#### `Window::session_name`

```cpp
[[nodiscard]] std::string_view session_name() const noexcept;
```
The owning psmux route carried by Windows live snapshots. Empty on POSIX and in recordings made from the backward-compatible `kFields` schema.

<a id="libtmux-entities-hpp-window-operator"></a>
#### `Window::operator==`

```cpp
[[nodiscard]] bool operator==(const Window& other) const noexcept;
```
Two values are the same window when they name the same tmux object on the same connection — not when they were listed at the same moment. A window refreshed after a rename equals the one it was refreshed from.

<a id="libtmux-entities-hpp-window-target"></a>
#### `Window::target`

```cpp
[[nodiscard]] std::string target() const;
```
How to address this window, and the reason a window id alone will not do.  The same window can be linked into several sessions, and a bare `@id` leaves tmux to pick one of those homes: the index it reports, the session it names, and the link a move or a kill lands on then depend on a choice the caller did not make. Qualifying by the session this window was listed from names one link.  Prefer `checked_target()`; this source-compatible form returns an empty string when psmux cannot bind a reusable target without a stale race.

<a id="libtmux-entities-hpp-window-checked-target"></a>
#### `Window::checked_target`

```cpp
[[nodiscard]] expected<std::string, CommandFailure> checked_target() const;
```

<a id="libtmux-entities-hpp-window-session"></a>
#### `Window::session`

```cpp
[[nodiscard]] expected<Session, CommandFailure> session() const;
```

<a id="libtmux-entities-hpp-window-panes"></a>
#### `Window::panes`

```cpp
[[nodiscard]] expected<std::vector<Pane>, CommandFailure> panes() const;
```

<a id="libtmux-entities-hpp-window-active-pane"></a>
#### `Window::active_pane`

```cpp
[[nodiscard]] expected<Pane, CommandFailure> active_pane() const;
```

<a id="libtmux-entities-hpp-window-split"></a>
#### `Window::split`

```cpp
[[nodiscard]] expected<Pane, CommandFailure> split() const;
```

<a id="libtmux-entities-hpp-window-split-2"></a>
#### `Window::split`

```cpp
[[nodiscard]] expected<Pane, CommandFailure> split(SplitOptions options) const;
```

<a id="libtmux-entities-hpp-window-rename"></a>
#### `Window::rename`

```cpp
[[nodiscard]] expected<void, CommandFailure> rename(std::string_view name) const;
```

<a id="libtmux-entities-hpp-window-select-layout"></a>
#### `Window::select_layout`

```cpp
[[nodiscard]] expected<void, CommandFailure> select_layout(std::string_view layout) const;
```
Rearrange the panes. tmux names five layouts and also accepts the layout description `layout()` returns, which is how a saved arrangement is restored exactly.

<a id="libtmux-entities-hpp-window-resize"></a>
#### `Window::resize`

```cpp
[[nodiscard]] expected<void, CommandFailure> resize(long long width, long long height) const;
```

<a id="libtmux-entities-hpp-window-next-layout"></a>
#### `Window::next_layout`

```cpp
[[nodiscard]] expected<void, CommandFailure> next_layout() const;
```
Exchange positions with another window, keeping both ids. Step through tmux's preset arrangements, and turn the panes within the one in use.  The layouts are tmux's list, not a caller's: asking for "the next one" is the only way to reach them without naming each. Rotating is a different act — it moves which pane occupies which cell and leaves the cells where they are.  None of the three refuses a window holding a single pane. tmux accepts all of them there and changes nothing, which is worth knowing before treating success as evidence that something moved.

<a id="libtmux-entities-hpp-window-previous-layout"></a>
#### `Window::previous_layout`

```cpp
[[nodiscard]] expected<void, CommandFailure> previous_layout() const;
```

<a id="libtmux-entities-hpp-window-rotate"></a>
#### `Window::rotate`

```cpp
[[nodiscard]] expected<void, CommandFailure> rotate() const;
```

<a id="libtmux-entities-hpp-window-select-last-pane"></a>
#### `Window::select_last_pane`

```cpp
[[nodiscard]] expected<Pane, CommandFailure> select_last_pane() const;
```
Go back to the pane that was selected before the current one, and answer with it.  Server state, like the window equivalent: nothing in a listing says which pane that was. A window holding one pane is refused rather than reselecting it, which is what tmux does.

<a id="libtmux-entities-hpp-window-link-to"></a>
#### `Window::link_to`

```cpp
[[nodiscard]] expected<void, CommandFailure> link_to(const Session& target) const;
```
Show this window in another session as well. The same window, not a copy: what runs in it is running in one place and shown in two.

<a id="libtmux-entities-hpp-window-unlink"></a>
#### `Window::unlink`

```cpp
[[nodiscard]] expected<void, CommandFailure> unlink() const;
```
Stop showing it in the session this value came from.  tmux refuses to remove the last link rather than leaving a window no session holds, and that refusal is kept: a caller who meant to be rid of the window wants `kill`, which says so.

<a id="libtmux-entities-hpp-window-swap-with"></a>
#### `Window::swap_with`

```cpp
[[nodiscard]] expected<void, CommandFailure> swap_with(const Window& other) const;
```

<a id="libtmux-entities-hpp-window-move-to"></a>
#### `Window::move_to`

```cpp
[[nodiscard]] expected<void, CommandFailure> move_to(long long index) const;
```
Move to another index within the same session.

<a id="libtmux-entities-hpp-window-expand"></a>
#### `Window::expand`

```cpp
[[nodiscard]] expected<std::string, CommandFailure> expand(std::string_view format) const;
```
Ask tmux to expand a format against this window: the same reach the session form gives, guarded the same way. A window that has gone reports `missing` rather than answering with a blank.

<a id="libtmux-entities-hpp-window-show-message"></a>
#### `Window::show_message`

```cpp
[[nodiscard]] expected<void, CommandFailure> show_message(std::string_view text) const;
```
Show a message to the clients watching this window's session.  The window is the context the text expands in, not just who sees it: `#{window_name}` in a message sent from here names this window even while another is the active one.

<a id="libtmux-entities-hpp-window-select"></a>
#### `Window::select`

```cpp
[[nodiscard]] expected<void, CommandFailure> select() const;
```

<a id="libtmux-entities-hpp-window-kill"></a>
#### `Window::kill`

```cpp
[[nodiscard]] expected<void, CommandFailure> kill() const;
```

<a id="libtmux-entities-hpp-window-refresh"></a>
#### `Window::refresh`

```cpp
[[nodiscard]] expected<Window, CommandFailure> refresh() const;
```

<a id="libtmux-entities-hpp-window-options"></a>
#### `Window::options`

```cpp
[[nodiscard]] expected<std::vector<OptionEntry>, CommandFailure> options() const;
```
Window options. tmux looks a named option up in the table it belongs to, so the scope here selects which window provides the context and the inheritance chain, not which names are legal.

<a id="libtmux-entities-hpp-window-option"></a>
#### `Window::option`

```cpp
[[nodiscard]] expected<OptionEntry, CommandFailure> option(std::string_view name) const;
```

<a id="libtmux-entities-hpp-window-set-option"></a>
#### `Window::set_option`

```cpp
[[nodiscard]] expected<void, CommandFailure> set_option(std::string_view name, std::string_view value) const;
```

<a id="libtmux-entities-hpp-window-unset-option"></a>
#### `Window::unset_option`

```cpp
[[nodiscard]] expected<void, CommandFailure> unset_option(std::string_view name) const;
```

<a id="libtmux-entities-hpp-pane"></a>
### `Pane`

```cpp
class Pane;
```

<a id="libtmux-entities-hpp-pane-knoun"></a>
#### `Pane::kNoun`

```cpp
static constexpr std::string_view kNoun{"pane"};
```

<a id="libtmux-entities-hpp-pane-ksessionnamefield"></a>
#### `Pane::kSessionNameField`

```cpp
static constexpr std::string_view kSessionNameField{"session_name"};
```

<a id="libtmux-entities-hpp-pane-kfields"></a>
#### `Pane::kFields`

```cpp
static constexpr std::array kFields{ std::string_view{"pane_id"}, std::string_view{"pane_current_command"}, std::string_view{"pane_active"}, std::string_view{"window_id"}, std::string_view{"session_id"}, std::string_view{"pane_index"}, std::string_view{"pane_title"}, std::string_view{"pane_pid"}, std::string_view{"pane_tty"}, std::string_view{"pane_current_path"}, std::string_view{"pane_width"}, std::string_view{"pane_height"}, std::string_view{"pane_dead"}, std::string_view{"pane_in_mode"}, std::string_view{"pane_at_top"}, std::string_view{"pane_at_bottom"}, std::string_view{"pane_at_left"}, std::string_view{"pane_at_right"}, std::string_view{"pane_pipe"}};
```

<a id="libtmux-entities-hpp-pane-pane"></a>
#### `Pane::Pane`

```cpp
Pane(std::shared_ptr<const Snapshot> snapshot, std::size_t row) noexcept;
```

<a id="libtmux-entities-hpp-pane-connection-identity"></a>
#### `Pane::connection_identity`

```cpp
using Row::connection_identity;
```

<a id="libtmux-entities-hpp-pane-server"></a>
#### `Pane::server`

```cpp
using Row::server;
```

<a id="libtmux-entities-hpp-pane-id"></a>
#### `Pane::id`

```cpp
[[nodiscard]] std::string_view id() const noexcept;
```

<a id="libtmux-entities-hpp-pane-command"></a>
#### `Pane::command`

```cpp
[[nodiscard]] std::string_view command() const noexcept;
```
What is running in the pane now, which is not what started it.

<a id="libtmux-entities-hpp-pane-active"></a>
#### `Pane::active`

```cpp
[[nodiscard]] bool active() const noexcept;
```

<a id="libtmux-entities-hpp-pane-window-id"></a>
#### `Pane::window_id`

```cpp
[[nodiscard]] std::string_view window_id() const noexcept;
```

<a id="libtmux-entities-hpp-pane-session-id"></a>
#### `Pane::session_id`

```cpp
[[nodiscard]] std::string_view session_id() const noexcept;
```

<a id="libtmux-entities-hpp-pane-index"></a>
#### `Pane::index`

```cpp
[[nodiscard]] long long index() const noexcept;
```

<a id="libtmux-entities-hpp-pane-title"></a>
#### `Pane::title`

```cpp
[[nodiscard]] std::string_view title() const noexcept;
```

<a id="libtmux-entities-hpp-pane-pid"></a>
#### `Pane::pid`

```cpp
[[nodiscard]] long long pid() const noexcept;
```

<a id="libtmux-entities-hpp-pane-tty"></a>
#### `Pane::tty`

```cpp
[[nodiscard]] std::string_view tty() const noexcept;
```

<a id="libtmux-entities-hpp-pane-path"></a>
#### `Pane::path`

```cpp
[[nodiscard]] std::string_view path() const noexcept;
```

<a id="libtmux-entities-hpp-pane-width"></a>
#### `Pane::width`

```cpp
[[nodiscard]] long long width() const noexcept;
```

<a id="libtmux-entities-hpp-pane-height"></a>
#### `Pane::height`

```cpp
[[nodiscard]] long long height() const noexcept;
```

<a id="libtmux-entities-hpp-pane-dead"></a>
#### `Pane::dead`

```cpp
[[nodiscard]] bool dead() const noexcept;
```
A pane whose program exited while `remain-on-exit` kept it on screen.

<a id="libtmux-entities-hpp-pane-in-mode"></a>
#### `Pane::in_mode`

```cpp
[[nodiscard]] bool in_mode() const noexcept;
```
Copy mode and its relatives, in which sent keys move the cursor rather than reaching the program.

<a id="libtmux-entities-hpp-pane-at-top"></a>
#### `Pane::at_top`

```cpp
[[nodiscard]] bool at_top() const noexcept;
```

<a id="libtmux-entities-hpp-pane-at-bottom"></a>
#### `Pane::at_bottom`

```cpp
[[nodiscard]] bool at_bottom() const noexcept;
```

<a id="libtmux-entities-hpp-pane-at-left"></a>
#### `Pane::at_left`

```cpp
[[nodiscard]] bool at_left() const noexcept;
```

<a id="libtmux-entities-hpp-pane-at-right"></a>
#### `Pane::at_right`

```cpp
[[nodiscard]] bool at_right() const noexcept;
```

<a id="libtmux-entities-hpp-pane-piping"></a>
#### `Pane::piping`

```cpp
[[nodiscard]] bool piping() const noexcept;
```
Whether this pane's output is currently being copied to a command.

<a id="libtmux-entities-hpp-pane-session-name"></a>
#### `Pane::session_name`

```cpp
[[nodiscard]] std::string_view session_name() const noexcept;
```
The owning psmux route carried by Windows live snapshots. Empty on POSIX and in recordings made from the backward-compatible `kFields` schema.

<a id="libtmux-entities-hpp-pane-operator"></a>
#### `Pane::operator==`

```cpp
[[nodiscard]] bool operator==(const Pane& other) const noexcept;
```
Two values are the same pane when they name the same tmux object on the same connection — not when they were listed at the same moment. A pane refreshed after a rename equals the one it was refreshed from.

<a id="libtmux-entities-hpp-pane-window"></a>
#### `Pane::window`

```cpp
[[nodiscard]] expected<Window, CommandFailure> window() const;
```

<a id="libtmux-entities-hpp-pane-session"></a>
#### `Pane::session`

```cpp
[[nodiscard]] expected<Session, CommandFailure> session() const;
```

<a id="libtmux-entities-hpp-pane-send-text"></a>
#### `Pane::send_text`

```cpp
[[nodiscard]] expected<void, CommandFailure> send_text(std::string_view text) const;
```
Literal text, never interpreted as key names or formats, and never followed by a newline the caller did not ask for.

<a id="libtmux-entities-hpp-pane-send-key"></a>
#### `Pane::send_key`

```cpp
[[nodiscard]] expected<void, CommandFailure> send_key(std::string_view key) const;
```

<a id="libtmux-entities-hpp-pane-capture"></a>
#### `Pane::capture`

```cpp
[[nodiscard]] expected<std::string, CommandFailure> capture() const;
```
The visible contents, as tmux printed them. `capture_lines` frames it into lines, and takes a named string: the lines are views into it, so framing this return value directly is a compile error rather than a dangling read.  A pane's scrollback can be far larger than the default bound, and a capture that does not fit is reported rather than cut, so a caller reading history passes the size it is prepared to hold.

<a id="libtmux-entities-hpp-pane-capture-2"></a>
#### `Pane::capture`

```cpp
[[nodiscard]] expected<std::string, CommandFailure> capture(CaptureOptions options) const;
```

<a id="libtmux-entities-hpp-pane-set-width"></a>
#### `Pane::set_width`

```cpp
[[nodiscard]] expected<void, CommandFailure> set_width(long long width) const;
```

<a id="libtmux-entities-hpp-pane-set-height"></a>
#### `Pane::set_height`

```cpp
[[nodiscard]] expected<void, CommandFailure> set_height(long long height) const;
```

<a id="libtmux-entities-hpp-pane-swap-with"></a>
#### `Pane::swap_with`

```cpp
[[nodiscard]] expected<void, CommandFailure> swap_with(const Pane& other) const;
```

<a id="libtmux-entities-hpp-pane-break-out"></a>
#### `Pane::break_out`

```cpp
[[nodiscard]] expected<Window, CommandFailure> break_out(std::string_view name = {}) const;
```
Take this pane out of its window and into a new one, which is returned. An empty name leaves tmux to name the window after what is running.

<a id="libtmux-entities-hpp-pane-join"></a>
#### `Pane::join`

```cpp
[[nodiscard]] expected<void, CommandFailure> join(const Window& target) const;
```
Move this pane into another window, splitting it. The other half of `break_out`: that takes the tree apart, this puts it back.  The pane keeps its id, so a value held across the move still names it. The window it came from disappears if it held nothing else, which is why the target is named rather than inferred from where this pane is.

<a id="libtmux-entities-hpp-pane-enter-copy-mode"></a>
#### `Pane::enter_copy_mode`

```cpp
[[nodiscard]] expected<void, CommandFailure> enter_copy_mode() const;
```
Forget the scrollback, which is the only way to bound a pane's memory without restarting what is running in it. Put this pane into copy mode, where its contents can be scrolled and selected rather than typed into.  Needs no attached client: the mode is pane state, which `in_mode` reports. Entering twice is harmless.

<a id="libtmux-entities-hpp-pane-leave-mode"></a>
#### `Pane::leave_mode`

```cpp
[[nodiscard]] expected<void, CommandFailure> leave_mode() const;
```
Leave whatever mode the pane is in.  A pane in no mode is refused, with tmux's "not in a mode". That is kept rather than smoothed into success: checking first would cost a round trip and still race, and a caller who cares can read `in_mode` or ignore the failure.

<a id="libtmux-entities-hpp-pane-pipe-to"></a>
#### `Pane::pipe_to`

```cpp
[[nodiscard]] expected<void, CommandFailure> pipe_to(std::string_view command) const;
```
Copy everything this pane prints to a shell command, until told to stop. The command runs on the tmux server's machine with the pane's output on its standard input.  Starting a second pipe replaces the first: tmux keeps one per pane, so there is nothing to close and nothing to leak.

<a id="libtmux-entities-hpp-pane-stop-piping"></a>
#### `Pane::stop_piping`

```cpp
[[nodiscard]] expected<void, CommandFailure> stop_piping() const;
```
Stop copying. Harmless on a pane that was not piping.

<a id="libtmux-entities-hpp-pane-set-title"></a>
#### `Pane::set_title`

```cpp
[[nodiscard]] expected<void, CommandFailure> set_title(std::string_view title) const;
```
Name this pane. The title is what `#{pane_title}` reports and what a status line can show; it survives the process being replaced.

<a id="libtmux-entities-hpp-pane-respawn"></a>
#### `Pane::respawn`

```cpp
[[nodiscard]] expected<void, CommandFailure> respawn(bool replace_running = false) const;
```
Start the pane's command again.  tmux refuses a pane whose process is still running unless told to kill it, and that refusal is kept rather than smoothed over: replacing a live process is a decision, so `replace_running` has to be asked for.

<a id="libtmux-entities-hpp-pane-clear-history"></a>
#### `Pane::clear_history`

```cpp
[[nodiscard]] expected<void, CommandFailure> clear_history() const;
```

<a id="libtmux-entities-hpp-pane-expand"></a>
#### `Pane::expand`

```cpp
[[nodiscard]] expected<std::string, CommandFailure> expand(std::string_view format) const;
```
Ask tmux to expand a format against this pane. `#{pane_current_command}` and `#{pane_current_path}` are the two most callers reach for, and neither is a field this class carries: both change under a value that stays still.  Guarded like the session and window forms, because tmux answers a pane that has gone with a blank and a zero exit status.

<a id="libtmux-entities-hpp-pane-show-message"></a>
#### `Pane::show_message`

```cpp
[[nodiscard]] expected<void, CommandFailure> show_message(std::string_view text) const;
```
Show a message to the clients watching this pane's session, expanded against this pane.

<a id="libtmux-entities-hpp-pane-paste"></a>
#### `Pane::paste`

```cpp
[[nodiscard]] expected<void, CommandFailure> paste(const Buffer& buffer, bool consume = false) const;
```
Deliver a buffer's text to this pane, as if it had been typed. The text arrives on the command line and is not run: a caller wanting it executed sends Enter afterwards, which is the same distinction `send_text` draws.  `consume` is tmux's `-d`, deleting the buffer once it has been pasted, which is what a caller treating it as a one-shot transfer wants.

<a id="libtmux-entities-hpp-pane-select"></a>
#### `Pane::select`

```cpp
[[nodiscard]] expected<void, CommandFailure> select() const;
```

<a id="libtmux-entities-hpp-pane-kill"></a>
#### `Pane::kill`

```cpp
[[nodiscard]] expected<void, CommandFailure> kill() const;
```

<a id="libtmux-entities-hpp-pane-refresh"></a>
#### `Pane::refresh`

```cpp
[[nodiscard]] expected<Pane, CommandFailure> refresh() const;
```

<a id="libtmux-entities-hpp-pane-options"></a>
#### `Pane::options`

```cpp
[[nodiscard]] expected<std::vector<OptionEntry>, CommandFailure> options() const;
```
Pane options, the narrowest scope tmux has, and the end of an inheritance chain that runs pane, window, session, global.

<a id="libtmux-entities-hpp-pane-option"></a>
#### `Pane::option`

```cpp
[[nodiscard]] expected<OptionEntry, CommandFailure> option(std::string_view name) const;
```

<a id="libtmux-entities-hpp-pane-set-option"></a>
#### `Pane::set_option`

```cpp
[[nodiscard]] expected<void, CommandFailure> set_option(std::string_view name, std::string_view value) const;
```

<a id="libtmux-entities-hpp-pane-unset-option"></a>
#### `Pane::unset_option`

```cpp
[[nodiscard]] expected<void, CommandFailure> unset_option(std::string_view name) const;
```

<a id="libtmux-entities-hpp-command"></a>
### `Command`

One command this tmux understands.  The list is how a caller asks what the server can do rather than deducing it from a version string. A build with commands compiled out, or a version between releases, answers for itself.

```cpp
class Command;
```

<a id="libtmux-entities-hpp-command-knoun"></a>
#### `Command::kNoun`

```cpp
static constexpr std::string_view kNoun{"command"};
```

<a id="libtmux-entities-hpp-command-kfields"></a>
#### `Command::kFields`

```cpp
static constexpr std::array kFields{std::string_view{"command_list_name"}, std::string_view{"command_list_alias"}, std::string_view{"command_list_usage"}};
```

<a id="libtmux-entities-hpp-command-command"></a>
#### `Command::Command`

```cpp
Command(std::shared_ptr<const Snapshot> snapshot, std::size_t row) noexcept;
```

<a id="libtmux-entities-hpp-command-connection-identity"></a>
#### `Command::connection_identity`

```cpp
using Row::connection_identity;
```

<a id="libtmux-entities-hpp-command-server"></a>
#### `Command::server`

```cpp
using Row::server;
```

<a id="libtmux-entities-hpp-command-name"></a>
#### `Command::name`

```cpp
[[nodiscard]] std::string_view name() const noexcept;
```

<a id="libtmux-entities-hpp-command-alias"></a>
#### `Command::alias`

```cpp
[[nodiscard]] std::string_view alias() const noexcept;
```
tmux's short form, such as `lscm` for `list-commands`. Empty when the command has none.

<a id="libtmux-entities-hpp-command-usage"></a>
#### `Command::usage`

```cpp
[[nodiscard]] std::string_view usage() const noexcept;
```
The flags and arguments, as tmux prints them in its own help.

<a id="libtmux-entities-hpp-command-operator"></a>
#### `Command::operator==`

```cpp
[[nodiscard]] bool operator==(const Command& other) const noexcept;
```

<a id="libtmux-entities-hpp-buffer"></a>
### `Buffer`

A named piece of text the server holds, outliving the pane it came from.  tmux's cut buffers are the clipboard between panes: a pane's selection lands in one, and pasting reads one back. They belong to the server, not to any pane, which is why they are listed from it.

```cpp
class Buffer;
```

<a id="libtmux-entities-hpp-buffer-knoun"></a>
#### `Buffer::kNoun`

```cpp
static constexpr std::string_view kNoun{"buffer"};
```

<a id="libtmux-entities-hpp-buffer-kfields"></a>
#### `Buffer::kFields`

```cpp
static constexpr std::array kFields{ std::string_view{"buffer_name"}, std::string_view{"buffer_size"}, std::string_view{"buffer_sample"}, std::string_view{"buffer_created"}};
```

<a id="libtmux-entities-hpp-buffer-buffer"></a>
#### `Buffer::Buffer`

```cpp
Buffer(std::shared_ptr<const Snapshot> snapshot, std::size_t row) noexcept;
```

<a id="libtmux-entities-hpp-buffer-connection-identity"></a>
#### `Buffer::connection_identity`

```cpp
using Row::connection_identity;
```

<a id="libtmux-entities-hpp-buffer-server"></a>
#### `Buffer::server`

```cpp
using Row::server;
```

<a id="libtmux-entities-hpp-buffer-name"></a>
#### `Buffer::name`

```cpp
[[nodiscard]] std::string_view name() const noexcept;
```
Named by the caller, or by tmux as `buffer0` and upward when it is not.

<a id="libtmux-entities-hpp-buffer-size"></a>
#### `Buffer::size`

```cpp
[[nodiscard]] long long size() const noexcept;
```

<a id="libtmux-entities-hpp-buffer-sample"></a>
#### `Buffer::sample`

```cpp
[[nodiscard]] std::string_view sample() const noexcept;
```
The opening of the contents, as tmux prints it in a listing. Truncated, and with control characters rendered — `contents()` reads the bytes.

<a id="libtmux-entities-hpp-buffer-created"></a>
#### `Buffer::created`

```cpp
[[nodiscard]] std::chrono::sys_seconds created() const noexcept;
```

<a id="libtmux-entities-hpp-buffer-operator"></a>
#### `Buffer::operator==`

```cpp
[[nodiscard]] bool operator==(const Buffer& other) const noexcept;
```

<a id="libtmux-entities-hpp-buffer-contents"></a>
#### `Buffer::contents`

```cpp
[[nodiscard]] expected<std::string, CommandFailure> contents() const;
```
The whole contents. tmux prints them with no trailing newline, so what comes back is exactly what was put in.

<a id="libtmux-entities-hpp-buffer-remove"></a>
#### `Buffer::remove`

```cpp
[[nodiscard]] expected<void, CommandFailure> remove() const;
```

<a id="libtmux-entities-hpp-client"></a>
### `Client`

```cpp
class Client;
```

<a id="libtmux-entities-hpp-client-knoun"></a>
#### `Client::kNoun`

```cpp
static constexpr std::string_view kNoun{"client"};
```

<a id="libtmux-entities-hpp-client-kfields"></a>
#### `Client::kFields`

```cpp
static constexpr std::array kFields{ std::string_view{"client_name"}, std::string_view{"client_session"}, std::string_view{"client_readonly"}, std::string_view{"client_tty"}, std::string_view{"client_width"}, std::string_view{"client_height"}, std::string_view{"client_created"}, std::string_view{"client_activity"}, std::string_view{"client_termname"}, std::string_view{"client_control_mode"}};
```

<a id="libtmux-entities-hpp-client-client"></a>
#### `Client::Client`

```cpp
Client(std::shared_ptr<const Snapshot> snapshot, std::size_t row) noexcept;
```

<a id="libtmux-entities-hpp-client-connection-identity"></a>
#### `Client::connection_identity`

```cpp
using Row::connection_identity;
```

<a id="libtmux-entities-hpp-client-server"></a>
#### `Client::server`

```cpp
using Row::server;
```

<a id="libtmux-entities-hpp-client-name"></a>
#### `Client::name`

```cpp
[[nodiscard]] std::string_view name() const noexcept;
```
A client is named by its terminal path, which is the only stable handle tmux gives; there is no client id format.

<a id="libtmux-entities-hpp-client-session-name"></a>
#### `Client::session_name`

```cpp
[[nodiscard]] std::string_view session_name() const noexcept;
```

<a id="libtmux-entities-hpp-client-read-only"></a>
#### `Client::read_only`

```cpp
[[nodiscard]] bool read_only() const noexcept;
```

<a id="libtmux-entities-hpp-client-tty"></a>
#### `Client::tty`

```cpp
[[nodiscard]] std::string_view tty() const noexcept;
```

<a id="libtmux-entities-hpp-client-width"></a>
#### `Client::width`

```cpp
[[nodiscard]] long long width() const noexcept;
```

<a id="libtmux-entities-hpp-client-height"></a>
#### `Client::height`

```cpp
[[nodiscard]] long long height() const noexcept;
```

<a id="libtmux-entities-hpp-client-created"></a>
#### `Client::created`

```cpp
[[nodiscard]] std::chrono::sys_seconds created() const noexcept;
```

<a id="libtmux-entities-hpp-client-last-activity"></a>
#### `Client::last_activity`

```cpp
[[nodiscard]] std::chrono::sys_seconds last_activity() const noexcept;
```

<a id="libtmux-entities-hpp-client-terminal"></a>
#### `Client::terminal`

```cpp
[[nodiscard]] std::string_view terminal() const noexcept;
```

<a id="libtmux-entities-hpp-client-control-mode"></a>
#### `Client::control_mode`

```cpp
[[nodiscard]] bool control_mode() const noexcept;
```
A control-mode client is a program driving tmux, not a terminal.

<a id="libtmux-entities-hpp-client-operator"></a>
#### `Client::operator==`

```cpp
[[nodiscard]] bool operator==(const Client& other) const noexcept;
```
Two values are the same client when they name the same terminal on the same connection.

<a id="libtmux-entities-hpp-client-session"></a>
#### `Client::session`

```cpp
[[nodiscard]] expected<Session, CommandFailure> session() const;
```

<a id="libtmux-entities-hpp-client-switch-to"></a>
#### `Client::switch_to`

```cpp
[[nodiscard]] expected<void, CommandFailure> switch_to(const Session& session) const;
```
Point this client at another session, leaving it attached.

<a id="libtmux-entities-hpp-client-detach"></a>
#### `Client::detach`

```cpp
[[nodiscard]] expected<void, CommandFailure> detach() const;
```

<a id="libtmux-entities-hpp-client-refresh"></a>
#### `Client::refresh`

```cpp
[[nodiscard]] expected<void, CommandFailure> refresh() const;
```
Redraw, and tell tmux the size this client is now, which matters for a control-mode client whose size tmux cannot otherwise observe.

<a id="libtmux-entities-hpp-std-hash-libtmux-session"></a>
### `std::hash<libtmux::Session>`

Hashing an entity uses exactly what its equality compares, so values from separate listings key an unordered container consistently.

```cpp
template <> struct std::hash<libtmux::Session>;
```

<a id="libtmux-entities-hpp-std-hash-libtmux-session-operator"></a>
#### `std::hash<libtmux::Session>::operator()`

```cpp
[[nodiscard]] std::size_t operator()(const libtmux::Session& value) const noexcept;
```

<a id="libtmux-entities-hpp-std-hash-libtmux-window"></a>
### `std::hash<libtmux::Window>`

```cpp
template <> struct std::hash<libtmux::Window>;
```

<a id="libtmux-entities-hpp-std-hash-libtmux-window-operator"></a>
#### `std::hash<libtmux::Window>::operator()`

```cpp
[[nodiscard]] std::size_t operator()(const libtmux::Window& value) const noexcept;
```

<a id="libtmux-entities-hpp-std-hash-libtmux-pane"></a>
### `std::hash<libtmux::Pane>`

```cpp
template <> struct std::hash<libtmux::Pane>;
```

<a id="libtmux-entities-hpp-std-hash-libtmux-pane-operator"></a>
#### `std::hash<libtmux::Pane>::operator()`

```cpp
[[nodiscard]] std::size_t operator()(const libtmux::Pane& value) const noexcept;
```

<a id="libtmux-entities-hpp-std-hash-libtmux-client"></a>
### `std::hash<libtmux::Client>`

```cpp
template <> struct std::hash<libtmux::Client>;
```

<a id="libtmux-entities-hpp-std-hash-libtmux-client-operator"></a>
#### `std::hash<libtmux::Client>::operator()`

```cpp
[[nodiscard]] std::size_t operator()(const libtmux::Client& value) const noexcept;
```

<a id="libtmux-entities-hpp-free-symbols"></a>
### `Free symbols`

<a id="libtmux-entities-hpp-free-symbols-operator"></a>
#### `operator<<`

```cpp
std::ostream& operator<<(std::ostream& stream, const Session& session);
```
Written as tmux would name it, with the detail that identifies it: an id and the thing a reader recognises it by. Declared against a forward-declared stream so no consumer pays for <ostream> to include an entity.

<a id="libtmux-entities-hpp-free-symbols-operator-2"></a>
#### `operator<<`

```cpp
std::ostream& operator<<(std::ostream& stream, const Window& window);
```

<a id="libtmux-entities-hpp-free-symbols-operator-3"></a>
#### `operator<<`

```cpp
std::ostream& operator<<(std::ostream& stream, const Pane& pane);
```

<a id="libtmux-entities-hpp-free-symbols-operator-4"></a>
#### `operator<<`

```cpp
std::ostream& operator<<(std::ostream& stream, const Client& client);
```

<a id="libtmux-entities-hpp-free-symbols-session-id"></a>
#### `session::id`

```cpp
inline constexpr StringFieldHandle<Session> id{ {Session::kFields[0], [](const Session& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-session-name"></a>
#### `session::name`

```cpp
inline constexpr StringFieldHandle<Session> name{ {Session::kFields[1], [](const Session& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-session-attached"></a>
#### `session::attached`

```cpp
inline constexpr BoolFieldHandle<Session> attached{ {Session::kFields[2], [](const Session& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-session-path"></a>
#### `session::path`

```cpp
inline constexpr StringFieldHandle<Session> path{ {Session::kFields[4], [](const Session& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-session-group"></a>
#### `session::group`

```cpp
inline constexpr StringFieldHandle<Session> group{ {Session::kFields[6], [](const Session& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-session-grouped"></a>
#### `session::grouped`

```cpp
inline constexpr BoolFieldHandle<Session> grouped{ {Session::kFields[7], [](const Session& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-session-client-count"></a>
#### `session::client_count`

```cpp
inline constexpr NumberFieldHandle<Session> client_count{ {Session::kFields[2], [](const Session& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-session-window-count"></a>
#### `session::window_count`

```cpp
inline constexpr NumberFieldHandle<Session> window_count{ {Session::kFields[3], [](const Session& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-window-id"></a>
#### `window::id`

```cpp
inline constexpr StringFieldHandle<Window> id{ {Window::kFields[0], [](const Window& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-window-name"></a>
#### `window::name`

```cpp
inline constexpr StringFieldHandle<Window> name{ {Window::kFields[1], [](const Window& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-window-active"></a>
#### `window::active`

```cpp
inline constexpr BoolFieldHandle<Window> active{ {Window::kFields[2], [](const Window& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-window-session-id"></a>
#### `window::session_id`

```cpp
inline constexpr StringFieldHandle<Window> session_id{ {Window::kFields[3], [](const Window& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-window-session-name"></a>
#### `window::session_name`

```cpp
inline constexpr StringFieldHandle<Window> session_name{ {Window::kSessionNameField, [](const Window& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-window-layout"></a>
#### `window::layout`

```cpp
inline constexpr StringFieldHandle<Window> layout{ {Window::kFields[8], [](const Window& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-window-zoomed"></a>
#### `window::zoomed`

```cpp
inline constexpr BoolFieldHandle<Window> zoomed{ {Window::kFields[9], [](const Window& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-window-bell"></a>
#### `window::bell`

```cpp
inline constexpr BoolFieldHandle<Window> bell{ {Window::kFields[10], [](const Window& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-window-activity"></a>
#### `window::activity`

```cpp
inline constexpr BoolFieldHandle<Window> activity{ {Window::kFields[11], [](const Window& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-window-index"></a>
#### `window::index`

```cpp
inline constexpr NumberFieldHandle<Window> index{ {Window::kFields[4], [](const Window& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-window-pane-count"></a>
#### `window::pane_count`

```cpp
inline constexpr NumberFieldHandle<Window> pane_count{ {Window::kFields[5], [](const Window& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-window-width"></a>
#### `window::width`

```cpp
inline constexpr NumberFieldHandle<Window> width{ {Window::kFields[6], [](const Window& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-window-height"></a>
#### `window::height`

```cpp
inline constexpr NumberFieldHandle<Window> height{ {Window::kFields[7], [](const Window& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-pane-id"></a>
#### `pane::id`

```cpp
inline constexpr StringFieldHandle<Pane> id{ {Pane::kFields[0], [](const Pane& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-pane-command"></a>
#### `pane::command`

```cpp
inline constexpr StringFieldHandle<Pane> command{ {Pane::kFields[1], [](const Pane& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-pane-active"></a>
#### `pane::active`

```cpp
inline constexpr BoolFieldHandle<Pane> active{ {Pane::kFields[2], [](const Pane& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-pane-window-id"></a>
#### `pane::window_id`

```cpp
inline constexpr StringFieldHandle<Pane> window_id{ {Pane::kFields[3], [](const Pane& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-pane-session-id"></a>
#### `pane::session_id`

```cpp
inline constexpr StringFieldHandle<Pane> session_id{ {Pane::kFields[4], [](const Pane& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-pane-session-name"></a>
#### `pane::session_name`

```cpp
inline constexpr StringFieldHandle<Pane> session_name{ {Pane::kSessionNameField, [](const Pane& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-pane-title"></a>
#### `pane::title`

```cpp
inline constexpr StringFieldHandle<Pane> title{ {Pane::kFields[6], [](const Pane& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-pane-tty"></a>
#### `pane::tty`

```cpp
inline constexpr StringFieldHandle<Pane> tty{ {Pane::kFields[8], [](const Pane& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-pane-path"></a>
#### `pane::path`

```cpp
inline constexpr StringFieldHandle<Pane> path{ {Pane::kFields[9], [](const Pane& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-pane-dead"></a>
#### `pane::dead`

```cpp
inline constexpr BoolFieldHandle<Pane> dead{ {Pane::kFields[12], [](const Pane& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-pane-in-mode"></a>
#### `pane::in_mode`

```cpp
inline constexpr BoolFieldHandle<Pane> in_mode{ {Pane::kFields[13], [](const Pane& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-pane-index"></a>
#### `pane::index`

```cpp
inline constexpr NumberFieldHandle<Pane> index{ {Pane::kFields[5], [](const Pane& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-pane-pid"></a>
#### `pane::pid`

```cpp
inline constexpr NumberFieldHandle<Pane> pid{ {Pane::kFields[7], [](const Pane& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-pane-width"></a>
#### `pane::width`

```cpp
inline constexpr NumberFieldHandle<Pane> width{ {Pane::kFields[10], [](const Pane& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-pane-height"></a>
#### `pane::height`

```cpp
inline constexpr NumberFieldHandle<Pane> height{ {Pane::kFields[11], [](const Pane& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-client-name"></a>
#### `client::name`

```cpp
inline constexpr StringFieldHandle<Client> name{ {Client::kFields[0], [](const Client& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-client-session-name"></a>
#### `client::session_name`

```cpp
inline constexpr StringFieldHandle<Client> session_name{ {Client::kFields[1], [](const Client& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-client-read-only"></a>
#### `client::read_only`

```cpp
inline constexpr BoolFieldHandle<Client> read_only{ {Client::kFields[2], [](const Client& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-client-tty"></a>
#### `client::tty`

```cpp
inline constexpr StringFieldHandle<Client> tty{ {Client::kFields[3], [](const Client& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-client-terminal"></a>
#### `client::terminal`

```cpp
inline constexpr StringFieldHandle<Client> terminal{ {Client::kFields[8], [](const Client& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-client-control-mode"></a>
#### `client::control_mode`

```cpp
inline constexpr BoolFieldHandle<Client> control_mode{ {Client::kFields[9], [](const Client& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-client-width"></a>
#### `client::width`

```cpp
inline constexpr NumberFieldHandle<Client> width{ {Client::kFields[4], [](const Client& row) { /* implementation omitted */ }}};
```

<a id="libtmux-entities-hpp-free-symbols-client-height"></a>
#### `client::height`

```cpp
inline constexpr NumberFieldHandle<Client> height{ {Client::kFields[5], [](const Client& row) { /* implementation omitted */ }}};
```

<a id="libtmux-snapshot-hpp"></a>
## `libtmux/snapshot.hpp`

tmux format requests and the snapshots their output becomes.  A snapshot is everything one tmux listing returned: the bytes, the rows parsed out of them, and the connection that produced them. Entities are a shared pointer to one of these plus a row index, which is why an entity can be copied, stored and returned with no owner to keep alive, and why iterating a filtered range never spawns tmux — the process ran once, when the snapshot was taken.  A snapshot is a moment. Nothing in it changes when tmux does; asking for current state means taking another one.

**Symbols:**

- [`FormatArgument`](#libtmux-snapshot-hpp-formatargument)
  - [`FormatArgument::flag`](#libtmux-snapshot-hpp-formatargument-flag)
  - [`FormatArgument::message`](#libtmux-snapshot-hpp-formatargument-message)
- [`Snapshot`](#libtmux-snapshot-hpp-snapshot)
  - [`Snapshot::take`](#libtmux-snapshot-hpp-snapshot-take)
  - [`Snapshot::take_in_session`](#libtmux-snapshot-hpp-snapshot-take-in-session)
  - [`Snapshot::take_in_session`](#libtmux-snapshot-hpp-snapshot-take-in-session-2)
  - [`Snapshot::from_recording`](#libtmux-snapshot-hpp-snapshot-from-recording)
  - [`Snapshot::Snapshot`](#libtmux-snapshot-hpp-snapshot-snapshot)
  - [`Snapshot::operator=`](#libtmux-snapshot-hpp-snapshot-operator)
  - [`Snapshot::Snapshot`](#libtmux-snapshot-hpp-snapshot-snapshot-2)
  - [`Snapshot::operator=`](#libtmux-snapshot-hpp-snapshot-operator-2)
  - [`Snapshot::~Snapshot`](#libtmux-snapshot-hpp-snapshot-snapshot-3)
  - [`Snapshot::index_of`](#libtmux-snapshot-hpp-snapshot-index-of)
  - [`Snapshot::rows`](#libtmux-snapshot-hpp-snapshot-rows)
  - [`Snapshot::backend`](#libtmux-snapshot-hpp-snapshot-backend)
- [`Free symbols`](#libtmux-snapshot-hpp-free-symbols)
  - [`kFormatSeparator`](#libtmux-snapshot-hpp-free-symbols-kformatseparator)
  - [`kFormatEscape`](#libtmux-snapshot-hpp-free-symbols-kformatescape)
  - [`format_request`](#libtmux-snapshot-hpp-free-symbols-format-request)
  - [`decode_value`](#libtmux-snapshot-hpp-free-symbols-decode-value)
  - [`split_row`](#libtmux-snapshot-hpp-free-symbols-split-row)

<a id="libtmux-snapshot-hpp-formatargument"></a>
### `FormatArgument`

Where a tmux subcommand wants its format string. Most take `-F`; `display-message` takes the format as its message argument instead, and passing `-F` to it addresses a different thing entirely.

```cpp
enum class FormatArgument;
```

<a id="libtmux-snapshot-hpp-formatargument-flag"></a>
#### `FormatArgument::flag` — `flag,`

<a id="libtmux-snapshot-hpp-formatargument-message"></a>
#### `FormatArgument::message` — `message,`

<a id="libtmux-snapshot-hpp-snapshot"></a>
### `Snapshot`

Owning row storage, shared by every entity taken from it.

```cpp
class Snapshot;
```

<a id="libtmux-snapshot-hpp-snapshot-take"></a>
#### `Snapshot::take`

```cpp
[[nodiscard]] static expected<std::shared_ptr<const Snapshot>, CommandFailure> take(std::shared_ptr<const detail::Backend> backend, std::span<const std::string_view> fields, std::vector<std::string> request, FormatArgument placement = FormatArgument::flag);
```
Run `request` with this entity's fields appended and parse what came back. Rows are parsed once, here: entities hold views into them, so a second parse would invalidate every entity already handed out.

<a id="libtmux-snapshot-hpp-snapshot-take-in-session"></a>
#### `Snapshot::take_in_session`

```cpp
[[nodiscard]] static expected<std::shared_ptr<const Snapshot>, CommandFailure> take_in_session(std::shared_ptr<const detail::Backend> backend, std::span<const std::string_view> fields, std::vector<std::string> request, FormatArgument placement, std::string_view session_id, std::string_view session_name);
```

<a id="libtmux-snapshot-hpp-snapshot-take-in-session-2"></a>
#### `Snapshot::take_in_session`

```cpp
[[nodiscard]] static expected<std::shared_ptr<const Snapshot>, CommandFailure> take_in_session(std::shared_ptr<const detail::Backend> backend, std::span<const std::string_view> fields, std::vector<std::string> request, FormatArgument placement, std::string_view session_id, std::string_view session_name, std::optional<std::chrono::milliseconds> timeout, std::optional<std::size_t> output_limit);
```

<a id="libtmux-snapshot-hpp-snapshot-from-recording"></a>
#### `Snapshot::from_recording`

```cpp
[[nodiscard]] static std::shared_ptr<const Snapshot> from_recording(std::span<const std::string_view> fields, std::string output);
```
Output that did not come from a live server: a recording, a fixture, a test. Entities read and filter exactly as they would from a listing, and anything that would run a command reports that there is no connection. Returns null if a row does not match the fields it claims to have.

<a id="libtmux-snapshot-hpp-snapshot-snapshot"></a>
#### `Snapshot::Snapshot`

```cpp
Snapshot(const Snapshot&) = delete;
```

<a id="libtmux-snapshot-hpp-snapshot-operator"></a>
#### `Snapshot::operator=`

```cpp
Snapshot& operator=(const Snapshot&) = delete;
```

<a id="libtmux-snapshot-hpp-snapshot-snapshot-2"></a>
#### `Snapshot::Snapshot`

```cpp
Snapshot(Snapshot&&) = delete;
```

<a id="libtmux-snapshot-hpp-snapshot-operator-2"></a>
#### `Snapshot::operator=`

```cpp
Snapshot& operator=(Snapshot&&) = delete;
```

<a id="libtmux-snapshot-hpp-snapshot-snapshot-3"></a>
#### `Snapshot::~Snapshot`

```cpp
~Snapshot();
```

<a id="libtmux-snapshot-hpp-snapshot-index-of"></a>
#### `Snapshot::index_of`

```cpp
[[nodiscard]] std::size_t index_of(std::string_view field) const noexcept;
```

<a id="libtmux-snapshot-hpp-snapshot-rows"></a>
#### `Snapshot::rows`

```cpp
[[nodiscard]] const std::vector<std::vector<std::string_view>>& rows() const noexcept;
```

<a id="libtmux-snapshot-hpp-snapshot-backend"></a>
#### `Snapshot::backend`

```cpp
[[nodiscard]] const std::shared_ptr<const detail::Backend>& backend() const noexcept;
```
The connection this came from, so an entity can act on what it describes.

<a id="libtmux-snapshot-hpp-free-symbols"></a>
### `Free symbols`

<a id="libtmux-snapshot-hpp-free-symbols-kformatseparator"></a>
#### `kFormatSeparator`

```cpp
inline constexpr std::string_view kFormatSeparator = "␞";
```
tmux joins requested formats with a separator that cannot appear in a format name. U+241E matches the Python implementation's default so both can read the same recorded output.  It is multi-byte, so every tmux this library starts is passed `-u`: a tmux that believes the terminal is not UTF-8 substitutes an underscore and no row splits at all.

<a id="libtmux-snapshot-hpp-free-symbols-kformatescape"></a>
#### `kFormatEscape`

```cpp
inline constexpr std::string_view kFormatEscape = "␛";
```
A separator absent from every format *name* is not absent from every format *value*, which is the whole difficulty. `tmux rename-window 'a␞b'` is accepted, and one such name used to make every window and pane listing on that server fail to split — data the caller never chose breaking reads of everything else.  So tmux escapes the separator before it can be mistaken for one. U+241B pairs with it and is escaped in turn, which is what makes the transform reversible: `␛S` is a separator that was in the value and `␛E` is an escape marker that was, and neither has a second reading.

<a id="libtmux-snapshot-hpp-free-symbols-format-request"></a>
#### `format_request`

```cpp
[[nodiscard]] inline std::string format_request(std::span<const std::string_view> fields);
```
Build the format argument for one entity's fields, terminating every field so a trailing empty value is still a value rather than a missing column.  The two substitutions nest rather than run in sequence, because tmux applies the inner one to the raw value and the outer one to its result. In that order a value already holding the escape marker is neutralised before the separator pass can produce one; reversed, the two become indistinguishable.  `#{s/…/…/:…}` predates every tmux this library supports, and neither character is a regular-expression metacharacter.  Unconditional, rather than applied only to the fields that could carry a separator. Expanding the substitutions costs about 0.32us per row — a 61-row listing pays 19us, against a process launch of some milliseconds — and a per-field exemption list is a thing to get wrong later, once, silently.

<a id="libtmux-snapshot-hpp-free-symbols-decode-value"></a>
#### `decode_value`

```cpp
[[nodiscard]] inline std::size_t decode_value(char* begin, std::size_t size) noexcept;
```
Undo that escaping, in place.  Escaping only ever lengthens, so the decoded bytes fit where the encoded ones were: the write cursor never overtakes the read cursor, nothing moves, and nothing is allocated. Answers the decoded length.  An escape marker followed by anything else is left as written. Output this library asked for contains no such sequence, and failing on one would mean a recording could not carry a literal `␛`.

<a id="libtmux-snapshot-hpp-free-symbols-split-row"></a>
#### `split_row`

```cpp
[[nodiscard]] inline bool split_row(std::string_view line, std::size_t fields, std::vector<std::string_view>& values);
```
Split one tmux output line into its field values.  The trailing separator emitted by `format_request` produces one empty tail element, which is dropped; a short or long row is reported rather than padded so a format-name typo cannot masquerade as an empty field.

<a id="libtmux-filter-expr-hpp"></a>
## `libtmux/filter_expr.hpp`

Value-semantic filter expressions over explicit snapshots.  An expression owns every operand it compares against, so it can outlive the call that built it and can be stored, copied, and later translated. It is deliberately not an expression template: the node set is a closed variant, so the same value that filters a range in memory can be inspected and, later, compiled to a tmux `-f` format string.

**Symbols:**

- [`StringOp`](#libtmux-filter-expr-hpp-stringop)
  - [`StringOp::equals`](#libtmux-filter-expr-hpp-stringop-equals)
  - [`StringOp::iequals`](#libtmux-filter-expr-hpp-stringop-iequals)
  - [`StringOp::contains`](#libtmux-filter-expr-hpp-stringop-contains)
  - [`StringOp::starts_with`](#libtmux-filter-expr-hpp-stringop-starts-with)
  - [`StringOp::ends_with`](#libtmux-filter-expr-hpp-stringop-ends-with)
- [`NumberOp`](#libtmux-filter-expr-hpp-numberop)
  - [`NumberOp::equals`](#libtmux-filter-expr-hpp-numberop-equals)
  - [`NumberOp::not_equals`](#libtmux-filter-expr-hpp-numberop-not-equals)
  - [`NumberOp::less`](#libtmux-filter-expr-hpp-numberop-less)
  - [`NumberOp::less_equal`](#libtmux-filter-expr-hpp-numberop-less-equal)
  - [`NumberOp::greater`](#libtmux-filter-expr-hpp-numberop-greater)
  - [`NumberOp::greater_equal`](#libtmux-filter-expr-hpp-numberop-greater-equal)
- [`Combine`](#libtmux-filter-expr-hpp-combine)
  - [`Combine::conjunction`](#libtmux-filter-expr-hpp-combine-conjunction)
  - [`Combine::disjunction`](#libtmux-filter-expr-hpp-combine-disjunction)
- [`StringField`](#libtmux-filter-expr-hpp-stringfield)
  - [`StringField::name`](#libtmux-filter-expr-hpp-stringfield-name)
  - [`StringField::string_view`](#libtmux-filter-expr-hpp-stringfield-string-view)
- [`BoolField`](#libtmux-filter-expr-hpp-boolfield)
  - [`BoolField::name`](#libtmux-filter-expr-hpp-boolfield-name)
  - [`BoolField::bool`](#libtmux-filter-expr-hpp-boolfield-bool)
- [`NumberField`](#libtmux-filter-expr-hpp-numberfield)
  - [`NumberField::name`](#libtmux-filter-expr-hpp-numberfield-name)
  - [`NumberField::long`](#libtmux-filter-expr-hpp-numberfield-long)
- [`FilterExpr`](#libtmux-filter-expr-hpp-filterexpr)
  - [`FilterExpr::Node`](#libtmux-filter-expr-hpp-filterexpr-node)
  - [`FilterExpr::FilterExpr`](#libtmux-filter-expr-hpp-filterexpr-filterexpr)
  - [`FilterExpr::FilterExpr`](#libtmux-filter-expr-hpp-filterexpr-filterexpr-2)
  - [`FilterExpr::operator=`](#libtmux-filter-expr-hpp-filterexpr-operator)
  - [`FilterExpr::FilterExpr`](#libtmux-filter-expr-hpp-filterexpr-filterexpr-3)
  - [`FilterExpr::operator=`](#libtmux-filter-expr-hpp-filterexpr-operator-2)
  - [`FilterExpr::~FilterExpr`](#libtmux-filter-expr-hpp-filterexpr-filterexpr-4)
  - [`FilterExpr::operator()`](#libtmux-filter-expr-hpp-filterexpr-operator-3)
  - [`FilterExpr::node`](#libtmux-filter-expr-hpp-filterexpr-node-2)
- [`FilterExpr::StringTest`](#libtmux-filter-expr-hpp-filterexpr-stringtest)
  - [`FilterExpr::StringTest::field`](#libtmux-filter-expr-hpp-filterexpr-stringtest-field)
  - [`FilterExpr::StringTest::op`](#libtmux-filter-expr-hpp-filterexpr-stringtest-op)
  - [`FilterExpr::StringTest::operand`](#libtmux-filter-expr-hpp-filterexpr-stringtest-operand)
- [`FilterExpr::BoolTest`](#libtmux-filter-expr-hpp-filterexpr-booltest)
  - [`FilterExpr::BoolTest::field`](#libtmux-filter-expr-hpp-filterexpr-booltest-field)
  - [`FilterExpr::BoolTest::expected`](#libtmux-filter-expr-hpp-filterexpr-booltest-expected)
- [`FilterExpr::NumberTest`](#libtmux-filter-expr-hpp-filterexpr-numbertest)
  - [`FilterExpr::NumberTest::field`](#libtmux-filter-expr-hpp-filterexpr-numbertest-field)
  - [`FilterExpr::NumberTest::op`](#libtmux-filter-expr-hpp-filterexpr-numbertest-op)
  - [`FilterExpr::NumberTest::operand`](#libtmux-filter-expr-hpp-filterexpr-numbertest-operand)
- [`FilterExpr::Group`](#libtmux-filter-expr-hpp-filterexpr-group)
  - [`FilterExpr::Group::combine`](#libtmux-filter-expr-hpp-filterexpr-group-combine)
  - [`FilterExpr::Group::operands`](#libtmux-filter-expr-hpp-filterexpr-group-operands)
- [`FilterExpr::Negation`](#libtmux-filter-expr-hpp-filterexpr-negation)
  - [`FilterExpr::Negation::operand`](#libtmux-filter-expr-hpp-filterexpr-negation-operand)
- [`FilterExpr::RelationTest`](#libtmux-filter-expr-hpp-filterexpr-relationtest)
  - [`FilterExpr::RelationTest::relation`](#libtmux-filter-expr-hpp-filterexpr-relationtest-relation)
  - [`FilterExpr::RelationTest::quantifier`](#libtmux-filter-expr-hpp-filterexpr-relationtest-quantifier)
  - [`FilterExpr::RelationTest::bool`](#libtmux-filter-expr-hpp-filterexpr-relationtest-bool)
  - [`FilterExpr::RelationTest::child`](#libtmux-filter-expr-hpp-filterexpr-relationtest-child)
- [`StringFieldHandle`](#libtmux-filter-expr-hpp-stringfieldhandle)
  - [`StringFieldHandle::field`](#libtmux-filter-expr-hpp-stringfieldhandle-field)
  - [`StringFieldHandle::operator==`](#libtmux-filter-expr-hpp-stringfieldhandle-operator)
  - [`StringFieldHandle::iequals`](#libtmux-filter-expr-hpp-stringfieldhandle-iequals)
  - [`StringFieldHandle::contains`](#libtmux-filter-expr-hpp-stringfieldhandle-contains)
  - [`StringFieldHandle::starts_with`](#libtmux-filter-expr-hpp-stringfieldhandle-starts-with)
  - [`StringFieldHandle::ends_with`](#libtmux-filter-expr-hpp-stringfieldhandle-ends-with)
- [`NumberFieldHandle`](#libtmux-filter-expr-hpp-numberfieldhandle)
  - [`NumberFieldHandle::field`](#libtmux-filter-expr-hpp-numberfieldhandle-field)
  - [`NumberFieldHandle::operator==`](#libtmux-filter-expr-hpp-numberfieldhandle-operator)
  - [`NumberFieldHandle::operator!=`](#libtmux-filter-expr-hpp-numberfieldhandle-operator-2)
  - [`NumberFieldHandle::operator<`](#libtmux-filter-expr-hpp-numberfieldhandle-operator-3)
  - [`NumberFieldHandle::operator<=`](#libtmux-filter-expr-hpp-numberfieldhandle-operator-4)
  - [`NumberFieldHandle::operator>`](#libtmux-filter-expr-hpp-numberfieldhandle-operator-5)
  - [`NumberFieldHandle::operator>=`](#libtmux-filter-expr-hpp-numberfieldhandle-operator-6)
- [`BoolFieldHandle`](#libtmux-filter-expr-hpp-boolfieldhandle)
  - [`BoolFieldHandle::field`](#libtmux-filter-expr-hpp-boolfieldhandle-field)
  - [`BoolFieldHandle::operatorFilterExpr<Entity>`](#libtmux-filter-expr-hpp-boolfieldhandle-operatorfilterexpr-entity)
  - [`BoolFieldHandle::operator==`](#libtmux-filter-expr-hpp-boolfieldhandle-operator)
- [`Free symbols`](#libtmux-filter-expr-hpp-free-symbols)
  - [`operator&&`](#libtmux-filter-expr-hpp-free-symbols-operator)
  - [`operator||`](#libtmux-filter-expr-hpp-free-symbols-operator-2)
  - [`operator!`](#libtmux-filter-expr-hpp-free-symbols-operator-3)
  - [`operator&&`](#libtmux-filter-expr-hpp-free-symbols-operator-4)
  - [`operator&&`](#libtmux-filter-expr-hpp-free-symbols-operator-5)
  - [`operator||`](#libtmux-filter-expr-hpp-free-symbols-operator-6)
  - [`operator||`](#libtmux-filter-expr-hpp-free-symbols-operator-7)
  - [`operator!`](#libtmux-filter-expr-hpp-free-symbols-operator-8)
  - [`tmuxq::matching`](#libtmux-filter-expr-hpp-free-symbols-tmuxq-matching)
  - [`tmuxq::matching`](#libtmux-filter-expr-hpp-free-symbols-tmuxq-matching-2)
  - [`matching`](#libtmux-filter-expr-hpp-free-symbols-matching)

<a id="libtmux-filter-expr-hpp-stringop"></a>
### `StringOp`

```cpp
enum class StringOp;
```

<a id="libtmux-filter-expr-hpp-stringop-equals"></a>
#### `StringOp::equals` — `equals,`

<a id="libtmux-filter-expr-hpp-stringop-iequals"></a>
#### `StringOp::iequals` — `iequals,`

<a id="libtmux-filter-expr-hpp-stringop-contains"></a>
#### `StringOp::contains` — `contains,`

<a id="libtmux-filter-expr-hpp-stringop-starts-with"></a>
#### `StringOp::starts_with` — `starts_with,`

<a id="libtmux-filter-expr-hpp-stringop-ends-with"></a>
#### `StringOp::ends_with` — `ends_with,`

<a id="libtmux-filter-expr-hpp-numberop"></a>
### `NumberOp`

tmux renders a count, a size and an index as text. Comparing them as text puts "9" after "10", so a numeric field is its own kind with its own operations rather than a string field a caller must remember to convert.

```cpp
enum class NumberOp;
```

<a id="libtmux-filter-expr-hpp-numberop-equals"></a>
#### `NumberOp::equals` — `equals,`

<a id="libtmux-filter-expr-hpp-numberop-not-equals"></a>
#### `NumberOp::not_equals` — `not_equals,`

<a id="libtmux-filter-expr-hpp-numberop-less"></a>
#### `NumberOp::less` — `less,`

<a id="libtmux-filter-expr-hpp-numberop-less-equal"></a>
#### `NumberOp::less_equal` — `less_equal,`

<a id="libtmux-filter-expr-hpp-numberop-greater"></a>
#### `NumberOp::greater` — `greater,`

<a id="libtmux-filter-expr-hpp-numberop-greater-equal"></a>
#### `NumberOp::greater_equal` — `greater_equal,`

<a id="libtmux-filter-expr-hpp-combine"></a>
### `Combine`

```cpp
enum class Combine;
```

<a id="libtmux-filter-expr-hpp-combine-conjunction"></a>
#### `Combine::conjunction` — `conjunction,`

<a id="libtmux-filter-expr-hpp-combine-disjunction"></a>
#### `Combine::disjunction` — `disjunction,`

<a id="libtmux-filter-expr-hpp-stringfield"></a>
### `StringField`

A field is a named accessor: the name is what a future tmux-format lowering needs, the accessor is what in-memory evaluation needs.  The two halves are only as consistent as whoever paired them, and nothing in the type system pairs them. A field naming `pane_title` while reading `pane_current_command` evaluates one way in memory and lowers to another, and the built-in handles avoid that only by taking their name from the entity's own `kFields` array rather than spelling it again.  Which is enough here and not enough everywhere. A caller may build a field however they like and get whatever they built — their process, their predicate. What must not follow is a forged pairing crossing a process boundary, so a document arriving from anywhere else is resolved by name against what this library actually queries, and a name it does not know is refused rather than evaluated to nothing. That check lives with the wire format, in the JSON integration, because it is the boundary that needs it.

```cpp
template <typename Entity> struct StringField;
```

<a id="libtmux-filter-expr-hpp-stringfield-name"></a>
#### `StringField::name`

```cpp
std::string_view name;
```

<a id="libtmux-filter-expr-hpp-stringfield-string-view"></a>
#### `StringField::string_view`

```cpp
std::string_view (*read)(const Entity&);
```

<a id="libtmux-filter-expr-hpp-boolfield"></a>
### `BoolField`

```cpp
template <typename Entity> struct BoolField;
```

<a id="libtmux-filter-expr-hpp-boolfield-name"></a>
#### `BoolField::name`

```cpp
std::string_view name;
```

<a id="libtmux-filter-expr-hpp-boolfield-bool"></a>
#### `BoolField::bool`

```cpp
bool (*read)(const Entity&);
```

<a id="libtmux-filter-expr-hpp-numberfield"></a>
### `NumberField`

```cpp
template <typename Entity> struct NumberField;
```

<a id="libtmux-filter-expr-hpp-numberfield-name"></a>
#### `NumberField::name`

```cpp
std::string_view name;
```

<a id="libtmux-filter-expr-hpp-numberfield-long"></a>
#### `NumberField::long`

```cpp
long long (*read)(const Entity&);
```

<a id="libtmux-filter-expr-hpp-filterexpr"></a>
### `FilterExpr`

```cpp
template <typename Entity> class FilterExpr;
```

<a id="libtmux-filter-expr-hpp-filterexpr-node"></a>
#### `FilterExpr::Node`

```cpp
using Node = std::variant<StringTest, BoolTest, NumberTest, Group, Negation, RelationTest>;
```

<a id="libtmux-filter-expr-hpp-filterexpr-filterexpr"></a>
#### `FilterExpr::FilterExpr`

```cpp
explicit FilterExpr(Node node);
```

<a id="libtmux-filter-expr-hpp-filterexpr-filterexpr-2"></a>
#### `FilterExpr::FilterExpr`

```cpp
FilterExpr(const FilterExpr& other);
```

<a id="libtmux-filter-expr-hpp-filterexpr-operator"></a>
#### `FilterExpr::operator=`

```cpp
FilterExpr& operator=(const FilterExpr& other);
```

<a id="libtmux-filter-expr-hpp-filterexpr-filterexpr-3"></a>
#### `FilterExpr::FilterExpr`

```cpp
FilterExpr(FilterExpr&&) noexcept = default;
```

<a id="libtmux-filter-expr-hpp-filterexpr-operator-2"></a>
#### `FilterExpr::operator=`

```cpp
FilterExpr& operator=(FilterExpr&&) noexcept = default;
```

<a id="libtmux-filter-expr-hpp-filterexpr-filterexpr-4"></a>
#### `FilterExpr::~FilterExpr`

```cpp
~FilterExpr() = default;
```

<a id="libtmux-filter-expr-hpp-filterexpr-operator-3"></a>
#### `FilterExpr::operator()`

```cpp
[[nodiscard]] bool operator()(const Entity& entity) const;
```

<a id="libtmux-filter-expr-hpp-filterexpr-node-2"></a>
#### `FilterExpr::node`

```cpp
[[nodiscard]] const Node& node() const noexcept;
```

<a id="libtmux-filter-expr-hpp-filterexpr-stringtest"></a>
### `FilterExpr::StringTest`

```cpp
struct StringTest;
```

<a id="libtmux-filter-expr-hpp-filterexpr-stringtest-field"></a>
#### `FilterExpr::StringTest::field`

```cpp
StringField<Entity> field;
```

<a id="libtmux-filter-expr-hpp-filterexpr-stringtest-op"></a>
#### `FilterExpr::StringTest::op`

```cpp
StringOp op;
```

<a id="libtmux-filter-expr-hpp-filterexpr-stringtest-operand"></a>
#### `FilterExpr::StringTest::operand`

```cpp
std::string operand;
```

<a id="libtmux-filter-expr-hpp-filterexpr-booltest"></a>
### `FilterExpr::BoolTest`

```cpp
struct BoolTest;
```

<a id="libtmux-filter-expr-hpp-filterexpr-booltest-field"></a>
#### `FilterExpr::BoolTest::field`

```cpp
BoolField<Entity> field;
```

<a id="libtmux-filter-expr-hpp-filterexpr-booltest-expected"></a>
#### `FilterExpr::BoolTest::expected`

```cpp
bool expected;
```

<a id="libtmux-filter-expr-hpp-filterexpr-numbertest"></a>
### `FilterExpr::NumberTest`

```cpp
struct NumberTest;
```

<a id="libtmux-filter-expr-hpp-filterexpr-numbertest-field"></a>
#### `FilterExpr::NumberTest::field`

```cpp
NumberField<Entity> field;
```

<a id="libtmux-filter-expr-hpp-filterexpr-numbertest-op"></a>
#### `FilterExpr::NumberTest::op`

```cpp
NumberOp op;
```

<a id="libtmux-filter-expr-hpp-filterexpr-numbertest-operand"></a>
#### `FilterExpr::NumberTest::operand`

```cpp
long long operand;
```

<a id="libtmux-filter-expr-hpp-filterexpr-group"></a>
### `FilterExpr::Group`

```cpp
struct Group;
```

<a id="libtmux-filter-expr-hpp-filterexpr-group-combine"></a>
#### `FilterExpr::Group::combine`

```cpp
Combine combine;
```

<a id="libtmux-filter-expr-hpp-filterexpr-group-operands"></a>
#### `FilterExpr::Group::operands`

```cpp
std::vector<FilterExpr> operands;
```

<a id="libtmux-filter-expr-hpp-filterexpr-negation"></a>
### `FilterExpr::Negation`

```cpp
struct Negation;
```

<a id="libtmux-filter-expr-hpp-filterexpr-negation-operand"></a>
#### `FilterExpr::Negation::operand`

```cpp
std::unique_ptr<FilterExpr> operand;
```

<a id="libtmux-filter-expr-hpp-filterexpr-relationtest"></a>
### `FilterExpr::RelationTest`

A relation crosses to another entity type, so its child expression cannot live in this variant. The node keeps the relation name and quantifier for inspection and owns the evaluation by value.

```cpp
struct RelationTest;
```

<a id="libtmux-filter-expr-hpp-filterexpr-relationtest-relation"></a>
#### `FilterExpr::RelationTest::relation`

```cpp
std::string relation;
```

<a id="libtmux-filter-expr-hpp-filterexpr-relationtest-quantifier"></a>
#### `FilterExpr::RelationTest::quantifier`

```cpp
int quantifier;
```

<a id="libtmux-filter-expr-hpp-filterexpr-relationtest-bool"></a>
#### `FilterExpr::RelationTest::bool`

```cpp
std::function<bool(const Entity&)> evaluate;
```

<a id="libtmux-filter-expr-hpp-filterexpr-relationtest-child"></a>
#### `FilterExpr::RelationTest::child`

```cpp
LoweredExpression child;
```
The child compares another entity, so it cannot live in this variant. Its lowered form can, which is what keeps a relation translatable.

<a id="libtmux-filter-expr-hpp-stringfieldhandle"></a>
### `StringFieldHandle`

A typed field handle. Only the operations a field's type actually supports are declared, so `pane::active.starts_with(...)` is a compile error rather than a runtime surprise.

```cpp
template <typename Entity> struct StringFieldHandle;
```

<a id="libtmux-filter-expr-hpp-stringfieldhandle-field"></a>
#### `StringFieldHandle::field`

```cpp
StringField<Entity> field;
```

<a id="libtmux-filter-expr-hpp-stringfieldhandle-operator"></a>
#### `StringFieldHandle::operator==`

```cpp
[[nodiscard]] FilterExpr<Entity> operator==(std::string_view operand) const;
```

<a id="libtmux-filter-expr-hpp-stringfieldhandle-iequals"></a>
#### `StringFieldHandle::iequals`

```cpp
[[nodiscard]] FilterExpr<Entity> iequals(std::string_view operand) const;
```

<a id="libtmux-filter-expr-hpp-stringfieldhandle-contains"></a>
#### `StringFieldHandle::contains`

```cpp
[[nodiscard]] FilterExpr<Entity> contains(std::string_view operand) const;
```

<a id="libtmux-filter-expr-hpp-stringfieldhandle-starts-with"></a>
#### `StringFieldHandle::starts_with`

```cpp
[[nodiscard]] FilterExpr<Entity> starts_with(std::string_view operand) const;
```

<a id="libtmux-filter-expr-hpp-stringfieldhandle-ends-with"></a>
#### `StringFieldHandle::ends_with`

```cpp
[[nodiscard]] FilterExpr<Entity> ends_with(std::string_view operand) const;
```

<a id="libtmux-filter-expr-hpp-numberfieldhandle"></a>
### `NumberFieldHandle`

```cpp
template <typename Entity> struct NumberFieldHandle;
```

<a id="libtmux-filter-expr-hpp-numberfieldhandle-field"></a>
#### `NumberFieldHandle::field`

```cpp
NumberField<Entity> field;
```

<a id="libtmux-filter-expr-hpp-numberfieldhandle-operator"></a>
#### `NumberFieldHandle::operator==`

```cpp
[[nodiscard]] FilterExpr<Entity> operator==(long long operand) const;
```

<a id="libtmux-filter-expr-hpp-numberfieldhandle-operator-2"></a>
#### `NumberFieldHandle::operator!=`

```cpp
[[nodiscard]] FilterExpr<Entity> operator!=(long long operand) const;
```

<a id="libtmux-filter-expr-hpp-numberfieldhandle-operator-3"></a>
#### `NumberFieldHandle::operator<`

```cpp
[[nodiscard]] FilterExpr<Entity> operator<(long long operand) const;
```

<a id="libtmux-filter-expr-hpp-numberfieldhandle-operator-4"></a>
#### `NumberFieldHandle::operator<=`

```cpp
[[nodiscard]] FilterExpr<Entity> operator<=(long long operand) const;
```

<a id="libtmux-filter-expr-hpp-numberfieldhandle-operator-5"></a>
#### `NumberFieldHandle::operator>`

```cpp
[[nodiscard]] FilterExpr<Entity> operator>(long long operand) const;
```

<a id="libtmux-filter-expr-hpp-numberfieldhandle-operator-6"></a>
#### `NumberFieldHandle::operator>=`

```cpp
[[nodiscard]] FilterExpr<Entity> operator>=(long long operand) const;
```

<a id="libtmux-filter-expr-hpp-boolfieldhandle"></a>
### `BoolFieldHandle`

```cpp
template <typename Entity> struct BoolFieldHandle;
```

<a id="libtmux-filter-expr-hpp-boolfieldhandle-field"></a>
#### `BoolFieldHandle::field`

```cpp
BoolField<Entity> field;
```

<a id="libtmux-filter-expr-hpp-boolfieldhandle-operatorfilterexpr-entity"></a>
#### `BoolFieldHandle::operatorFilterExpr<Entity>`

```cpp
[[nodiscard]] operator FilterExpr<Entity>() const;
```

<a id="libtmux-filter-expr-hpp-boolfieldhandle-operator"></a>
#### `BoolFieldHandle::operator==`

```cpp
[[nodiscard]] FilterExpr<Entity> operator==(bool expected) const;
```

<a id="libtmux-filter-expr-hpp-free-symbols"></a>
### `Free symbols`

<a id="libtmux-filter-expr-hpp-free-symbols-operator"></a>
#### `operator&&`

```cpp
template <typename Entity> [[nodiscard]] FilterExpr<Entity> operator&&(FilterExpr<Entity> left, FilterExpr<Entity> right);
```

<a id="libtmux-filter-expr-hpp-free-symbols-operator-2"></a>
#### `operator||`

```cpp
template <typename Entity> [[nodiscard]] FilterExpr<Entity> operator||(FilterExpr<Entity> left, FilterExpr<Entity> right);
```

<a id="libtmux-filter-expr-hpp-free-symbols-operator-3"></a>
#### `operator!`

```cpp
template <typename Entity> [[nodiscard]] FilterExpr<Entity> operator!(FilterExpr<Entity> operand);
```

<a id="libtmux-filter-expr-hpp-free-symbols-operator-4"></a>
#### `operator&&`

```cpp
template <typename Entity> [[nodiscard]] FilterExpr<Entity> operator&&(FilterExpr<Entity> left, BoolFieldHandle<Entity> right);
```

<a id="libtmux-filter-expr-hpp-free-symbols-operator-5"></a>
#### `operator&&`

```cpp
template <typename Entity> [[nodiscard]] FilterExpr<Entity> operator&&(BoolFieldHandle<Entity> left, FilterExpr<Entity> right);
```

<a id="libtmux-filter-expr-hpp-free-symbols-operator-6"></a>
#### `operator||`

```cpp
template <typename Entity> [[nodiscard]] FilterExpr<Entity> operator||(FilterExpr<Entity> left, BoolFieldHandle<Entity> right);
```

<a id="libtmux-filter-expr-hpp-free-symbols-operator-7"></a>
#### `operator||`

```cpp
template <typename Entity> [[nodiscard]] FilterExpr<Entity> operator||(BoolFieldHandle<Entity> left, FilterExpr<Entity> right);
```

<a id="libtmux-filter-expr-hpp-free-symbols-operator-8"></a>
#### `operator!`

```cpp
template <typename Entity> [[nodiscard]] FilterExpr<Entity> operator!(BoolFieldHandle<Entity> operand);
```

<a id="libtmux-filter-expr-hpp-free-symbols-tmuxq-matching"></a>
#### `tmuxq::matching`

```cpp
template <typename Entity> [[nodiscard]] auto matching(FilterExpr<Entity> expr);
```
`matching(expr)` is a range adaptor closure so it composes with std views.

<a id="libtmux-filter-expr-hpp-free-symbols-tmuxq-matching-2"></a>
#### `tmuxq::matching`

```cpp
template <typename Entity> [[nodiscard]] auto matching(BoolFieldHandle<Entity> field);
```
A bare flag field is a complete question, so it adapts a range directly.

<a id="libtmux-filter-expr-hpp-free-symbols-matching"></a>
#### `matching`

```cpp
using tmuxq::matching;
```

<a id="libtmux-relations-hpp"></a>
## `libtmux/relations.hpp`

Relation quantifiers over to-many and to-one links.  The quantifier is named rather than inferred so a reader never has to guess what an empty relation means: `all_of` is satisfied by an empty relation, `any_of` is not, and `none_of` is. That is the vacuous-truth convention the standard algorithms already use, stated explicitly because getting it wrong silently changes which entities a filter returns.

**Symbols:**

- [`Quantifier`](#libtmux-relations-hpp-quantifier)
  - [`Quantifier::any_of`](#libtmux-relations-hpp-quantifier-any-of)
  - [`Quantifier::all_of`](#libtmux-relations-hpp-quantifier-all-of)
  - [`Quantifier::none_of`](#libtmux-relations-hpp-quantifier-none-of)
  - [`Quantifier::is`](#libtmux-relations-hpp-quantifier-is)
- [`Free symbols`](#libtmux-relations-hpp-free-symbols)
  - [`children_of`](#libtmux-relations-hpp-free-symbols-children-of)
  - [`children_of`](#libtmux-relations-hpp-free-symbols-children-of-2)
  - [`parent_of`](#libtmux-relations-hpp-free-symbols-parent-of)
  - [`RelatedMany`](#libtmux-relations-hpp-free-symbols-relatedmany)
  - [`RelatedOne`](#libtmux-relations-hpp-free-symbols-relatedone)
  - [`quantified`](#libtmux-relations-hpp-free-symbols-quantified)
  - [`any_of`](#libtmux-relations-hpp-free-symbols-any-of)
  - [`all_of`](#libtmux-relations-hpp-free-symbols-all-of)
  - [`none_of`](#libtmux-relations-hpp-free-symbols-none-of)
  - [`is`](#libtmux-relations-hpp-free-symbols-is)
  - [`parent_of`](#libtmux-relations-hpp-free-symbols-parent-of-2)

<a id="libtmux-relations-hpp-quantifier"></a>
### `Quantifier`

```cpp
enum class Quantifier;
```

<a id="libtmux-relations-hpp-quantifier-any-of"></a>
#### `Quantifier::any_of` — `any_of,`

<a id="libtmux-relations-hpp-quantifier-all-of"></a>
#### `Quantifier::all_of` — `all_of,`

<a id="libtmux-relations-hpp-quantifier-none-of"></a>
#### `Quantifier::none_of` — `none_of,`

<a id="libtmux-relations-hpp-quantifier-is"></a>
#### `Quantifier::is` — `is,`

<a id="libtmux-relations-hpp-free-symbols"></a>
### `Free symbols`

<a id="libtmux-relations-hpp-free-symbols-children-of"></a>
#### `children_of`

```cpp
template <typename Parent, typename Child> [[nodiscard]] auto children_of(const std::vector<Child>& rows, StringFieldHandle<Child> foreign_key, StringFieldHandle<Parent> key);
```
Join two listings on the id one of them carries.  A relation predicate has to reach the related rows without running tmux, so the caller lists both kinds once and links them here. The result borrows the rows it was given, exactly as a filtered view does, so it must not outlive the listing it reads.

<a id="libtmux-relations-hpp-free-symbols-children-of-2"></a>
#### `children_of`

```cpp
template <typename Parent, typename Child> auto children_of(const std::vector<Child>&&, StringFieldHandle<Child>, StringFieldHandle<Parent>) = delete;
```
The rows are borrowed, so a listing that dies at the semicolon is refused here rather than dangling inside the predicate later.

<a id="libtmux-relations-hpp-free-symbols-parent-of"></a>
#### `parent_of`

```cpp
template <typename Child, typename Parent> [[nodiscard]] auto parent_of(const std::vector<Parent>& rows, StringFieldHandle<Child> foreign_key, StringFieldHandle<Parent> key);
```
The same join read the other way: the one row a child points at, or none.

<a id="libtmux-relations-hpp-free-symbols-relatedmany"></a>
#### `RelatedMany`

```cpp
template <typename Entity, typename Read> using RelatedMany = std::ranges::range_value_t< std::remove_cvref_t<std::invoke_result_t<Read, const Entity&>>>;
```
The related entity is whatever the accessor yields, so a bare flag field can stand in for a predicate here exactly as it does in `matching`.

<a id="libtmux-relations-hpp-free-symbols-relatedone"></a>
#### `RelatedOne`

```cpp
template <typename Entity, typename Read> using RelatedOne = std::remove_cvref_t< std::remove_pointer_t<std::invoke_result_t<Read, const Entity&>>>;
```

<a id="libtmux-relations-hpp-free-symbols-quantified"></a>
#### `quantified`

```cpp
template <typename Entity> [[nodiscard]] FilterExpr<Entity> quantified(std::string name, Quantifier quantifier, auto read, auto predicate);
```
To-many and to-one links take different accessors, so they get different builders rather than one that has to compile both shapes.

<a id="libtmux-relations-hpp-free-symbols-any-of"></a>
#### `any_of`

```cpp
template <typename Entity> [[nodiscard]] FilterExpr<Entity> any_of(std::string name, auto read, auto predicate);
```

<a id="libtmux-relations-hpp-free-symbols-all-of"></a>
#### `all_of`

```cpp
template <typename Entity> [[nodiscard]] FilterExpr<Entity> all_of(std::string name, auto read, auto predicate);
```

<a id="libtmux-relations-hpp-free-symbols-none-of"></a>
#### `none_of`

```cpp
template <typename Entity> [[nodiscard]] FilterExpr<Entity> none_of(std::string name, auto read, auto predicate);
```

<a id="libtmux-relations-hpp-free-symbols-is"></a>
#### `is`

```cpp
template <typename Entity> [[nodiscard]] FilterExpr<Entity> is(std::string name, auto read, auto predicate);
```
An absent to-one link never satisfies `is`: a window with no active pane is not a window whose active pane runs an editor.

<a id="libtmux-relations-hpp-free-symbols-parent-of-2"></a>
#### `parent_of`

```cpp
template <typename Child, typename Parent> auto parent_of(const std::vector<Parent>&&, StringFieldHandle<Child>, StringFieldHandle<Parent>) = delete;
```

<a id="libtmux-cardinality-hpp"></a>
## `libtmux/cardinality.hpp`

Exception-free cardinality over snapshot views.  Callers ask for one entity far more often than they want to handle a range, and the two ways that request can fail are not the same failure: finding nothing is ordinary, finding several means the caller's filter was wrong. These return types keep both outcomes in the value channel.

**Symbols:**

- [`CardinalityError`](#libtmux-cardinality-hpp-cardinalityerror)
  - [`CardinalityError::none_matched`](#libtmux-cardinality-hpp-cardinalityerror-none-matched)
  - [`CardinalityError::several_matched`](#libtmux-cardinality-hpp-cardinalityerror-several-matched)
- [`Free symbols`](#libtmux-cardinality-hpp-free-symbols)
  - [`to_string`](#libtmux-cardinality-hpp-free-symbols-to-string)
  - [`Referenced`](#libtmux-cardinality-hpp-free-symbols-referenced)
  - [`ReferenceRange`](#libtmux-cardinality-hpp-free-symbols-referencerange)
  - [`first`](#libtmux-cardinality-hpp-free-symbols-first)
  - [`exactly_one`](#libtmux-cardinality-hpp-free-symbols-exactly-one)

<a id="libtmux-cardinality-hpp-cardinalityerror"></a>
### `CardinalityError`

```cpp
enum class CardinalityError;
```

<a id="libtmux-cardinality-hpp-cardinalityerror-none-matched"></a>
#### `CardinalityError::none_matched` — `none_matched,`

<a id="libtmux-cardinality-hpp-cardinalityerror-several-matched"></a>
#### `CardinalityError::several_matched` — `several_matched,`

<a id="libtmux-cardinality-hpp-free-symbols"></a>
### `Free symbols`

<a id="libtmux-cardinality-hpp-free-symbols-to-string"></a>
#### `to_string`

```cpp
[[nodiscard]] constexpr std::string_view to_string(CardinalityError error) noexcept;
```

<a id="libtmux-cardinality-hpp-free-symbols-referenced"></a>
#### `Referenced`

```cpp
template <std::ranges::input_range Range> using Referenced = std::reference_wrapper<const std::ranges::range_value_t<Range>>;
```

<a id="libtmux-cardinality-hpp-free-symbols-referencerange"></a>
#### `ReferenceRange`

```cpp
template <typename Range> concept ReferenceRange = std::ranges::input_range<Range> && std::is_lvalue_reference_v<std::ranges::range_reference_t<Range>>;
```
Both also require the range to yield references. A range whose elements are produced on demand — a transform that returns by value, say — has nothing for the answer to refer to, and the temporary dies with the call.

<a id="libtmux-cardinality-hpp-free-symbols-first"></a>
#### `first`

```cpp
template <ReferenceRange Range> [[nodiscard]] std::optional<Referenced<Range>> first(Range& range);
```
`first` states that a caller tolerates extras; it never reports several.

<a id="libtmux-cardinality-hpp-free-symbols-exactly-one"></a>
#### `exactly_one`

```cpp
template <ReferenceRange Range> [[nodiscard]] expected<Referenced<Range>, CardinalityError> exactly_one(Range& range);
```
`exactly_one` states that several is a caller error, and says which one.

<a id="libtmux-command-hpp"></a>
## `libtmux/command.hpp`

Why a tmux command produced no answer.  `refused` means tmux ran and said no; `missing` means tmux ran, said yes, and the object asked about was not there; `truncated` means it answered at greater length than the caller allowed for. `unsupported` is a backend feature gap; `validation` is a bad request, so callers handle them differently.

**Symbols:**

- [`FailureKind`](#libtmux-command-hpp-failurekind)
  - [`FailureKind::validation`](#libtmux-command-hpp-failurekind-validation)
  - [`FailureKind::spawn`](#libtmux-command-hpp-failurekind-spawn)
  - [`FailureKind::pre_exec`](#libtmux-command-hpp-failurekind-pre-exec)
  - [`FailureKind::pipe`](#libtmux-command-hpp-failurekind-pipe)
  - [`FailureKind::timeout`](#libtmux-command-hpp-failurekind-timeout)
  - [`FailureKind::refused`](#libtmux-command-hpp-failurekind-refused)
  - [`FailureKind::missing`](#libtmux-command-hpp-failurekind-missing)
  - [`FailureKind::truncated`](#libtmux-command-hpp-failurekind-truncated)
  - [`FailureKind::unsupported`](#libtmux-command-hpp-failurekind-unsupported)
- [`CommandFailure`](#libtmux-command-hpp-commandfailure)
  - [`CommandFailure::kind`](#libtmux-command-hpp-commandfailure-kind)
  - [`CommandFailure::dispatched`](#libtmux-command-hpp-commandfailure-dispatched)
  - [`CommandFailure::exit_code`](#libtmux-command-hpp-commandfailure-exit-code)
  - [`CommandFailure::diagnostic`](#libtmux-command-hpp-commandfailure-diagnostic)
- [`ExecutionPolicy`](#libtmux-command-hpp-executionpolicy)
  - [`ExecutionPolicy::timeout`](#libtmux-command-hpp-executionpolicy-timeout)
  - [`ExecutionPolicy::output_limit`](#libtmux-command-hpp-executionpolicy-output-limit)
- [`Free symbols`](#libtmux-command-hpp-free-symbols)
  - [`to_string`](#libtmux-command-hpp-free-symbols-to-string)
  - [`CommandObserver`](#libtmux-command-hpp-free-symbols-commandobserver)

<a id="libtmux-command-hpp-failurekind"></a>
### `FailureKind`

```cpp
enum class FailureKind;
```

<a id="libtmux-command-hpp-failurekind-validation"></a>
#### `FailureKind::validation` — `validation,`

<a id="libtmux-command-hpp-failurekind-spawn"></a>
#### `FailureKind::spawn` — `spawn,`

<a id="libtmux-command-hpp-failurekind-pre-exec"></a>
#### `FailureKind::pre_exec` — `pre_exec,`

<a id="libtmux-command-hpp-failurekind-pipe"></a>
#### `FailureKind::pipe` — `pipe,`

<a id="libtmux-command-hpp-failurekind-timeout"></a>
#### `FailureKind::timeout` — `timeout,`

<a id="libtmux-command-hpp-failurekind-refused"></a>
#### `FailureKind::refused` — `refused,`

<a id="libtmux-command-hpp-failurekind-missing"></a>
#### `FailureKind::missing` — `missing,`

<a id="libtmux-command-hpp-failurekind-truncated"></a>
#### `FailureKind::truncated` — `truncated,`

tmux ran and answered, and the answer did not fit. Reported rather than returned, because a truncated answer is indistinguishable from a complete one: the last line is simply cut, mid-word.

<a id="libtmux-command-hpp-failurekind-unsupported"></a>
#### `FailureKind::unsupported` — `unsupported,`

The backend cannot provide this operation without weakening its contract; nothing was dispatched.

<a id="libtmux-command-hpp-commandfailure"></a>
### `CommandFailure`

```cpp
struct CommandFailure;
```

<a id="libtmux-command-hpp-commandfailure-kind"></a>
#### `CommandFailure::kind`

```cpp
FailureKind kind{FailureKind::refused};
```

<a id="libtmux-command-hpp-commandfailure-dispatched"></a>
#### `CommandFailure::dispatched`

```cpp
bool dispatched{};
```
True only when tmux itself ran. Retrying a dispatched command repeats whatever it already did.

<a id="libtmux-command-hpp-commandfailure-exit-code"></a>
#### `CommandFailure::exit_code`

```cpp
int exit_code{};
```

<a id="libtmux-command-hpp-commandfailure-diagnostic"></a>
#### `CommandFailure::diagnostic`

```cpp
std::string diagnostic;
```

<a id="libtmux-command-hpp-executionpolicy"></a>
### `ExecutionPolicy`

What a call waits and holds when the caller did not say.  A timeout on the call is still how a caller says "this one in particular": listing sessions does not share a deadline with attaching a client. What was missing was a floor. Typed methods passed no timeout at all, so `window.rename(...)` waited for as long as the process ran if tmux never answered — and "tmux is normally fast" is not a liveness guarantee when a hook blocks, a filesystem stops answering, or a connection breaks without closing the pipe.  Thirty seconds is far past every tmux command that works and far short of forever. `wait_for` opts out, because waiting is the whole request.

```cpp
struct ExecutionPolicy;
```

<a id="libtmux-command-hpp-executionpolicy-timeout"></a>
#### `ExecutionPolicy::timeout`

```cpp
std::optional<std::chrono::milliseconds> timeout{std::chrono::seconds{30}};
```
Absent means wait. That is a thing to mean deliberately.

<a id="libtmux-command-hpp-executionpolicy-output-limit"></a>
#### `ExecutionPolicy::output_limit`

```cpp
std::optional<std::size_t> output_limit{};
```
Absent leaves the transport's own bound, which is one megabyte.

<a id="libtmux-command-hpp-free-symbols"></a>
### `Free symbols`

<a id="libtmux-command-hpp-free-symbols-to-string"></a>
#### `to_string`

```cpp
[[nodiscard]] constexpr std::string_view to_string(FailureKind kind) noexcept;
```

<a id="libtmux-command-hpp-free-symbols-commandobserver"></a>
#### `CommandObserver`

```cpp
using CommandObserver = std::function<void(std::string_view command, const CommandFailure* failure)>;
```
Told about every command, as it finishes.  There is otherwise no way to see what this library ran: a caller debugging a tmux interaction has only the failures, and nothing at all when things succeed. The command is rendered as tmux received it, with any argument marked sensitive replaced.  Called on the thread that ran the command, while nothing is held, so an observer that itself calls tmux does not deadlock — but one shared between threads has to say so itself.

<a id="libtmux-options-hpp"></a>
## `libtmux/options.hpp`

Parse `show-options`-shaped output.  tmux prints one option per line as `name value`, and an array option as `name[index] value`. The value may be quoted when it contains spaces, and an option with an empty value prints its name alone. Keeping all three shapes in one parser means callers never hand-split option output again.

**Symbols:**

- [`OptionEntry`](#libtmux-options-hpp-optionentry)
  - [`OptionEntry::name`](#libtmux-options-hpp-optionentry-name)
  - [`OptionEntry::index`](#libtmux-options-hpp-optionentry-index)
  - [`OptionEntry::value`](#libtmux-options-hpp-optionentry-value)
  - [`OptionEntry::inherited`](#libtmux-options-hpp-optionentry-inherited)
- [`Free symbols`](#libtmux-options-hpp-free-symbols)
  - [`unquote`](#libtmux-options-hpp-free-symbols-unquote)
  - [`parse_option`](#libtmux-options-hpp-free-symbols-parse-option)
  - [`parse_options`](#libtmux-options-hpp-free-symbols-parse-options)

<a id="libtmux-options-hpp-optionentry"></a>
### `OptionEntry`

```cpp
struct OptionEntry;
```

<a id="libtmux-options-hpp-optionentry-name"></a>
#### `OptionEntry::name`

```cpp
std::string name;
```

<a id="libtmux-options-hpp-optionentry-index"></a>
#### `OptionEntry::index`

```cpp
std::optional<std::size_t> index;
```
Present only for array options, which tmux indexes sparsely: absent indexes are genuinely unset rather than empty.

<a id="libtmux-options-hpp-optionentry-value"></a>
#### `OptionEntry::value`

```cpp
std::string value;
```

<a id="libtmux-options-hpp-optionentry-inherited"></a>
#### `OptionEntry::inherited`

```cpp
bool inherited{};
```
True when the value comes from a wider scope rather than being set here. `show-options -A` marks these with a trailing asterisk on the name.  tmux accepts a user option whose name itself ends in an asterisk, and prints `@star* value` for one set here — which is the same shape as `status-position* bottom` for an inherited option. In a listing the two cannot be told apart, and the marker reading wins. Asking for one option by name is exact, because the name asked for settles it.

<a id="libtmux-options-hpp-free-symbols"></a>
### `Free symbols`

<a id="libtmux-options-hpp-free-symbols-unquote"></a>
#### `unquote`

```cpp
[[nodiscard]] inline std::string unquote(std::string_view value);
```
Undo the quoting tmux applies when it prints a value.  tmux picks one of four forms, and a reader that knows only one corrupts the rest: `''` for an empty value; double quotes when the value contains any of ` #';${}`; single quotes when it contains a double quote; and otherwise no quotes at all. Inside any of them the body is escaped the way `vis` does it — `\t`, `\n`, `\\`, `\ooo` for a byte with no printable form — and a leading tilde is escaped whether or not anything else is.  This is the inverse of tmux's own `args_escape`, so a value read here and written back is the value that was there.

<a id="libtmux-options-hpp-free-symbols-parse-option"></a>
#### `parse_option`

```cpp
[[nodiscard]] inline std::optional<OptionEntry> parse_option(std::string_view line);
```
Parse one line. Returns nullopt for a blank line so callers can feed raw output straight in.

<a id="libtmux-options-hpp-free-symbols-parse-options"></a>
#### `parse_options`

```cpp
[[nodiscard]] inline std::vector<OptionEntry> parse_options(std::string_view output);
```

<a id="libtmux-control-hpp"></a>
## `libtmux/control.hpp`

Decode tmux's control protocol.  A control-mode stream interleaves command reply blocks with asynchronous notifications. This parser turns bytes into those events and nothing else: no threads, no process, no executor. Feeding it is the caller's job, which is what lets the same decoder serve a synchronous read loop today and an async executor later without either appearing in this header.  Framing preserves bytes. A line inside a block that looks like a notification stays block body, because control-mode framing does not make it independently attributable, and block bodies are never converted to UTF-8.

**Symbols:**

- [`ProtocolError`](#libtmux-control-hpp-protocolerror)
  - [`ProtocolError::message`](#libtmux-control-hpp-protocolerror-message)
- [`ControlTerminal`](#libtmux-control-hpp-controlterminal)
  - [`ControlTerminal::end`](#libtmux-control-hpp-controlterminal-end)
  - [`ControlTerminal::error`](#libtmux-control-hpp-controlterminal-error)
- [`ControlBlock`](#libtmux-control-hpp-controlblock)
  - [`ControlBlock::sequence`](#libtmux-control-hpp-controlblock-sequence)
  - [`ControlBlock::command_number`](#libtmux-control-hpp-controlblock-command-number)
  - [`ControlBlock::terminal`](#libtmux-control-hpp-controlblock-terminal)
  - [`ControlBlock::begin_metadata`](#libtmux-control-hpp-controlblock-begin-metadata)
  - [`ControlBlock::terminal_metadata`](#libtmux-control-hpp-controlblock-terminal-metadata)
  - [`ControlBlock::body`](#libtmux-control-hpp-controlblock-body)
  - [`ControlBlock::body_truncated`](#libtmux-control-hpp-controlblock-body-truncated)
  - [`ControlBlock::body_bytes`](#libtmux-control-hpp-controlblock-body-bytes)
- [`Notification`](#libtmux-control-hpp-notification)
  - [`Notification::body`](#libtmux-control-hpp-notification-body)
- [`NotificationKind`](#libtmux-control-hpp-notificationkind)
  - [`NotificationKind::unknown`](#libtmux-control-hpp-notificationkind-unknown)
  - [`NotificationKind::output`](#libtmux-control-hpp-notificationkind-output)
  - [`NotificationKind::extended_output`](#libtmux-control-hpp-notificationkind-extended-output)
  - [`NotificationKind::paused`](#libtmux-control-hpp-notificationkind-paused)
  - [`NotificationKind::resumed`](#libtmux-control-hpp-notificationkind-resumed)
  - [`NotificationKind::sessions_changed`](#libtmux-control-hpp-notificationkind-sessions-changed)
  - [`NotificationKind::session_changed`](#libtmux-control-hpp-notificationkind-session-changed)
  - [`NotificationKind::session_renamed`](#libtmux-control-hpp-notificationkind-session-renamed)
  - [`NotificationKind::session_window_changed`](#libtmux-control-hpp-notificationkind-session-window-changed)
  - [`NotificationKind::client_detached`](#libtmux-control-hpp-notificationkind-client-detached)
  - [`NotificationKind::client_session_changed`](#libtmux-control-hpp-notificationkind-client-session-changed)
  - [`NotificationKind::window_add`](#libtmux-control-hpp-notificationkind-window-add)
  - [`NotificationKind::window_close`](#libtmux-control-hpp-notificationkind-window-close)
  - [`NotificationKind::window_renamed`](#libtmux-control-hpp-notificationkind-window-renamed)
  - [`NotificationKind::window_pane_changed`](#libtmux-control-hpp-notificationkind-window-pane-changed)
  - [`NotificationKind::unlinked_window_add`](#libtmux-control-hpp-notificationkind-unlinked-window-add)
  - [`NotificationKind::unlinked_window_close`](#libtmux-control-hpp-notificationkind-unlinked-window-close)
  - [`NotificationKind::unlinked_window_renamed`](#libtmux-control-hpp-notificationkind-unlinked-window-renamed)
  - [`NotificationKind::pane_mode_changed`](#libtmux-control-hpp-notificationkind-pane-mode-changed)
  - [`NotificationKind::paste_buffer_changed`](#libtmux-control-hpp-notificationkind-paste-buffer-changed)
  - [`NotificationKind::paste_buffer_deleted`](#libtmux-control-hpp-notificationkind-paste-buffer-deleted)
  - [`NotificationKind::subscription_changed`](#libtmux-control-hpp-notificationkind-subscription-changed)
- [`ParsedNotification`](#libtmux-control-hpp-parsednotification)
  - [`ParsedNotification::kind`](#libtmux-control-hpp-parsednotification-kind)
  - [`ParsedNotification::name`](#libtmux-control-hpp-parsednotification-name)
  - [`ParsedNotification::session`](#libtmux-control-hpp-parsednotification-session)
  - [`ParsedNotification::window`](#libtmux-control-hpp-parsednotification-window)
  - [`ParsedNotification::pane`](#libtmux-control-hpp-parsednotification-pane)
  - [`ParsedNotification::text`](#libtmux-control-hpp-parsednotification-text)
  - [`ParsedNotification::payload`](#libtmux-control-hpp-parsednotification-payload)
  - [`ParsedNotification::age`](#libtmux-control-hpp-parsednotification-age)
- [`Parser`](#libtmux-control-hpp-parser)
  - [`Parser::Parser`](#libtmux-control-hpp-parser-parser)
  - [`Parser::Parser`](#libtmux-control-hpp-parser-parser-2)
  - [`Parser::feed`](#libtmux-control-hpp-parser-feed)
  - [`Parser::finish`](#libtmux-control-hpp-parser-finish)
- [`Attribution`](#libtmux-control-hpp-attribution)
  - [`Attribution::exact`](#libtmux-control-hpp-attribution-exact)
  - [`Attribution::skipped`](#libtmux-control-hpp-attribution-skipped)
  - [`Attribution::unknown`](#libtmux-control-hpp-attribution-unknown)
- [`ControlCommand`](#libtmux-control-hpp-controlcommand)
  - [`ControlCommand::argv`](#libtmux-control-hpp-controlcommand-argv)
- [`ControlRequest`](#libtmux-control-hpp-controlrequest)
  - [`ControlRequest::group`](#libtmux-control-hpp-controlrequest-group)
- [`ControlOperationResult`](#libtmux-control-hpp-controloperationresult)
  - [`ControlOperationResult::attribution`](#libtmux-control-hpp-controloperationresult-attribution)
  - [`ControlOperationResult::block`](#libtmux-control-hpp-controloperationresult-block)
- [`ControlRequestResult`](#libtmux-control-hpp-controlrequestresult)
  - [`ControlRequestResult::operations`](#libtmux-control-hpp-controlrequestresult-operations)
  - [`ControlRequestResult::connection_error`](#libtmux-control-hpp-controlrequestresult-connection-error)
- [`ConnectionOptions`](#libtmux-control-hpp-connectionoptions)
  - [`ConnectionOptions::tmux_binary`](#libtmux-control-hpp-connectionoptions-tmux-binary)
  - [`ConnectionOptions::socket_path`](#libtmux-control-hpp-connectionoptions-socket-path)
  - [`ConnectionOptions::session_name`](#libtmux-control-hpp-connectionoptions-session-name)
  - [`ConnectionOptions::startup_timeout`](#libtmux-control-hpp-connectionoptions-startup-timeout)
  - [`ConnectionOptions::shutdown_timeout`](#libtmux-control-hpp-connectionoptions-shutdown-timeout)
  - [`ConnectionOptions::retained_reply_bytes`](#libtmux-control-hpp-connectionoptions-retained-reply-bytes)
  - [`ConnectionOptions::line_bytes`](#libtmux-control-hpp-connectionoptions-line-bytes)
  - [`ConnectionOptions::pane_output`](#libtmux-control-hpp-connectionoptions-pane-output)
  - [`ConnectionOptions::pause_after`](#libtmux-control-hpp-connectionoptions-pause-after)
- [`NotificationRange`](#libtmux-control-hpp-notificationrange)
  - [`NotificationRange::NotificationRange`](#libtmux-control-hpp-notificationrange-notificationrange)
  - [`NotificationRange::begin`](#libtmux-control-hpp-notificationrange-begin)
  - [`NotificationRange::end`](#libtmux-control-hpp-notificationrange-end)
- [`NotificationRange::iterator`](#libtmux-control-hpp-notificationrange-iterator)
  - [`NotificationRange::iterator::difference_type`](#libtmux-control-hpp-notificationrange-iterator-difference-type)
  - [`NotificationRange::iterator::value_type`](#libtmux-control-hpp-notificationrange-iterator-value-type)
  - [`NotificationRange::iterator::iterator_concept`](#libtmux-control-hpp-notificationrange-iterator-iterator-concept)
  - [`NotificationRange::iterator::iterator`](#libtmux-control-hpp-notificationrange-iterator-iterator)
  - [`NotificationRange::iterator::iterator`](#libtmux-control-hpp-notificationrange-iterator-iterator-2)
  - [`NotificationRange::iterator::operator*`](#libtmux-control-hpp-notificationrange-iterator-operator)
  - [`NotificationRange::iterator::operator++`](#libtmux-control-hpp-notificationrange-iterator-operator-2)
  - [`NotificationRange::iterator::operator++`](#libtmux-control-hpp-notificationrange-iterator-operator-3)
  - [`NotificationRange::iterator::operator==`](#libtmux-control-hpp-notificationrange-iterator-operator-4)
- [`Connection`](#libtmux-control-hpp-connection)
  - [`Connection::connect`](#libtmux-control-hpp-connection-connect)
  - [`Connection::~Connection`](#libtmux-control-hpp-connection-connection)
  - [`Connection::Connection`](#libtmux-control-hpp-connection-connection-2)
  - [`Connection::operator=`](#libtmux-control-hpp-connection-operator)
  - [`Connection::Connection`](#libtmux-control-hpp-connection-connection-3)
  - [`Connection::operator=`](#libtmux-control-hpp-connection-operator-2)
  - [`Connection::execute`](#libtmux-control-hpp-connection-execute)
  - [`Connection::take_notifications`](#libtmux-control-hpp-connection-take-notifications)
  - [`Connection::wait_for_notifications`](#libtmux-control-hpp-connection-wait-for-notifications)
  - [`Connection::notification_fd`](#libtmux-control-hpp-connection-notification-fd)
  - [`Connection::set_pane_output`](#libtmux-control-hpp-connection-set-pane-output)
  - [`Connection::events`](#libtmux-control-hpp-connection-events)
  - [`Connection::dropped_notifications`](#libtmux-control-hpp-connection-dropped-notifications)
  - [`Connection::native_child_pid`](#libtmux-control-hpp-connection-native-child-pid)
  - [`Connection::shutdown`](#libtmux-control-hpp-connection-shutdown)
- [`Free symbols`](#libtmux-control-hpp-free-symbols)
  - [`kDefaultRetainedReplyBytes`](#libtmux-control-hpp-free-symbols-kdefaultretainedreplybytes)
  - [`kDefaultLineBytes`](#libtmux-control-hpp-free-symbols-kdefaultlinebytes)
  - [`Event`](#libtmux-control-hpp-free-symbols-event)
  - [`to_string`](#libtmux-control-hpp-free-symbols-to-string)
  - [`parse`](#libtmux-control-hpp-free-symbols-parse)
  - [`parse`](#libtmux-control-hpp-free-symbols-parse-2)

<a id="libtmux-control-hpp-protocolerror"></a>
### `ProtocolError`

Why a decode stopped. The stream is terminal for its connection: a caller cannot resynchronise a control stream, only start a new one.

```cpp
struct ProtocolError;
```

<a id="libtmux-control-hpp-protocolerror-message"></a>
#### `ProtocolError::message`

```cpp
std::string message;
```

<a id="libtmux-control-hpp-controlterminal"></a>
### `ControlTerminal`

```cpp
enum class ControlTerminal : std::uint8_t;
```

<a id="libtmux-control-hpp-controlterminal-end"></a>
#### `ControlTerminal::end` — `end,`

<a id="libtmux-control-hpp-controlterminal-error"></a>
#### `ControlTerminal::error` — `error,`

<a id="libtmux-control-hpp-controlblock"></a>
### `ControlBlock`

```cpp
struct ControlBlock;
```

<a id="libtmux-control-hpp-controlblock-sequence"></a>
#### `ControlBlock::sequence`

```cpp
std::uint64_t sequence;
```

<a id="libtmux-control-hpp-controlblock-command-number"></a>
#### `ControlBlock::command_number`

```cpp
std::uint64_t command_number;
```

<a id="libtmux-control-hpp-controlblock-terminal"></a>
#### `ControlBlock::terminal`

```cpp
ControlTerminal terminal;
```

<a id="libtmux-control-hpp-controlblock-begin-metadata"></a>
#### `ControlBlock::begin_metadata`

```cpp
std::vector<std::byte> begin_metadata;
```

<a id="libtmux-control-hpp-controlblock-terminal-metadata"></a>
#### `ControlBlock::terminal_metadata`

```cpp
std::vector<std::byte> terminal_metadata;
```

<a id="libtmux-control-hpp-controlblock-body"></a>
#### `ControlBlock::body`

```cpp
std::vector<std::byte> body;
```

<a id="libtmux-control-hpp-controlblock-body-truncated"></a>
#### `ControlBlock::body_truncated`

```cpp
bool body_truncated{false};
```
`body` holds the first `retained_reply_bytes` and stopped; `body_bytes` is how many there were. Set rather than reported as an error because framing is the parser's job and judging the answer is the caller's: the rest of the reply is still drained, so the next command's reply is still attributable.

<a id="libtmux-control-hpp-controlblock-body-bytes"></a>
#### `ControlBlock::body_bytes`

```cpp
std::size_t body_bytes{0};
```

<a id="libtmux-control-hpp-notification"></a>
### `Notification`

```cpp
struct Notification;
```

<a id="libtmux-control-hpp-notification-body"></a>
#### `Notification::body`

```cpp
std::vector<std::byte> body;
```

<a id="libtmux-control-hpp-notificationkind"></a>
### `NotificationKind`

What a notification is, once its name and arguments have been read.  `unknown` is not a failure. tmux has only ever added notifications across the range this library supports — nineteen at 3.2a, twenty-one from 3.4 — so a name this build does not know is a newer tmux, and the body is still there to read. That is also why this is a kind and fields rather than a variant: an exhaustive `std::visit` would turn every such addition into a caller-breaking change.

```cpp
enum class NotificationKind : std::uint8_t;
```

<a id="libtmux-control-hpp-notificationkind-unknown"></a>
#### `NotificationKind::unknown` — `unknown,`

<a id="libtmux-control-hpp-notificationkind-output"></a>
#### `NotificationKind::output` — `output,`

<a id="libtmux-control-hpp-notificationkind-extended-output"></a>
#### `NotificationKind::extended_output` — `extended_output,`

<a id="libtmux-control-hpp-notificationkind-paused"></a>
#### `NotificationKind::paused` — `paused,`

<a id="libtmux-control-hpp-notificationkind-resumed"></a>
#### `NotificationKind::resumed` — `resumed,`

<a id="libtmux-control-hpp-notificationkind-sessions-changed"></a>
#### `NotificationKind::sessions_changed` — `sessions_changed,`

<a id="libtmux-control-hpp-notificationkind-session-changed"></a>
#### `NotificationKind::session_changed` — `session_changed,`

<a id="libtmux-control-hpp-notificationkind-session-renamed"></a>
#### `NotificationKind::session_renamed` — `session_renamed,`

<a id="libtmux-control-hpp-notificationkind-session-window-changed"></a>
#### `NotificationKind::session_window_changed` — `session_window_changed,`

<a id="libtmux-control-hpp-notificationkind-client-detached"></a>
#### `NotificationKind::client_detached` — `client_detached,`

<a id="libtmux-control-hpp-notificationkind-client-session-changed"></a>
#### `NotificationKind::client_session_changed` — `client_session_changed,`

<a id="libtmux-control-hpp-notificationkind-window-add"></a>
#### `NotificationKind::window_add` — `window_add,`

<a id="libtmux-control-hpp-notificationkind-window-close"></a>
#### `NotificationKind::window_close` — `window_close,`

<a id="libtmux-control-hpp-notificationkind-window-renamed"></a>
#### `NotificationKind::window_renamed` — `window_renamed,`

<a id="libtmux-control-hpp-notificationkind-window-pane-changed"></a>
#### `NotificationKind::window_pane_changed` — `window_pane_changed,`

<a id="libtmux-control-hpp-notificationkind-unlinked-window-add"></a>
#### `NotificationKind::unlinked_window_add` — `unlinked_window_add,`

<a id="libtmux-control-hpp-notificationkind-unlinked-window-close"></a>
#### `NotificationKind::unlinked_window_close` — `unlinked_window_close,`

<a id="libtmux-control-hpp-notificationkind-unlinked-window-renamed"></a>
#### `NotificationKind::unlinked_window_renamed` — `unlinked_window_renamed,`

<a id="libtmux-control-hpp-notificationkind-pane-mode-changed"></a>
#### `NotificationKind::pane_mode_changed` — `pane_mode_changed,`

<a id="libtmux-control-hpp-notificationkind-paste-buffer-changed"></a>
#### `NotificationKind::paste_buffer_changed` — `paste_buffer_changed,`

<a id="libtmux-control-hpp-notificationkind-paste-buffer-deleted"></a>
#### `NotificationKind::paste_buffer_deleted` — `paste_buffer_deleted,`

<a id="libtmux-control-hpp-notificationkind-subscription-changed"></a>
#### `NotificationKind::subscription_changed` — `subscription_changed,`

<a id="libtmux-control-hpp-parsednotification"></a>
### `ParsedNotification`

A notification's arguments, as views into the notification it was read from.  tmux types its arguments by prefix — `$0` a session, `@1` a window, `%2` a pane — so each lands in the field it belongs to and the others stay empty. `payload` is the pane bytes of an output notification, already unescaped; it is empty for every other kind.  Everything here borrows. The notification must outlive it, which is why there is no overload taking a temporary.

```cpp
struct ParsedNotification;
```

<a id="libtmux-control-hpp-parsednotification-kind"></a>
#### `ParsedNotification::kind`

```cpp
NotificationKind kind{NotificationKind::unknown};
```

<a id="libtmux-control-hpp-parsednotification-name"></a>
#### `ParsedNotification::name`

```cpp
std::string_view name{};
```

<a id="libtmux-control-hpp-parsednotification-session"></a>
#### `ParsedNotification::session`

```cpp
std::string_view session{};
```

<a id="libtmux-control-hpp-parsednotification-window"></a>
#### `ParsedNotification::window`

```cpp
std::string_view window{};
```

<a id="libtmux-control-hpp-parsednotification-pane"></a>
#### `ParsedNotification::pane`

```cpp
std::string_view pane{};
```

<a id="libtmux-control-hpp-parsednotification-text"></a>
#### `ParsedNotification::text`

```cpp
std::string_view text{};
```

<a id="libtmux-control-hpp-parsednotification-payload"></a>
#### `ParsedNotification::payload`

```cpp
std::span<const std::byte> payload{};
```

<a id="libtmux-control-hpp-parsednotification-age"></a>
#### `ParsedNotification::age`

```cpp
std::optional<std::uint64_t> age{};
```
Milliseconds this output was behind when tmux wrote it. Only `extended_output` carries one.

<a id="libtmux-control-hpp-parser"></a>
### `Parser`

```cpp
class Parser final;
```

<a id="libtmux-control-hpp-parser-parser"></a>
#### `Parser::Parser`

```cpp
Parser() = default;
```

<a id="libtmux-control-hpp-parser-parser-2"></a>
#### `Parser::Parser`

```cpp
Parser(std::size_t retained_reply_bytes, std::size_t line_bytes) noexcept;
```
Zero means unbounded, which only a test that owns both ends should ask for.

<a id="libtmux-control-hpp-parser-feed"></a>
#### `Parser::feed`

```cpp
expected<std::vector<Event>, ProtocolError> feed(std::span<const std::byte> bytes);
```

<a id="libtmux-control-hpp-parser-finish"></a>
#### `Parser::finish`

```cpp
expected<void, ProtocolError> finish();
```

<a id="libtmux-control-hpp-attribution"></a>
### `Attribution`

```cpp
enum class Attribution : std::uint8_t;
```

<a id="libtmux-control-hpp-attribution-exact"></a>
#### `Attribution::exact` — `exact,`

<a id="libtmux-control-hpp-attribution-skipped"></a>
#### `Attribution::skipped` — `skipped,`

<a id="libtmux-control-hpp-attribution-unknown"></a>
#### `Attribution::unknown` — `unknown,`

<a id="libtmux-control-hpp-controlcommand"></a>
### `ControlCommand`

```cpp
struct ControlCommand;
```

<a id="libtmux-control-hpp-controlcommand-argv"></a>
#### `ControlCommand::argv`

```cpp
std::vector<std::string> argv;
```

<a id="libtmux-control-hpp-controlrequest"></a>
### `ControlRequest`

```cpp
struct ControlRequest;
```

<a id="libtmux-control-hpp-controlrequest-group"></a>
#### `ControlRequest::group`

```cpp
std::vector<ControlCommand> group;
```

<a id="libtmux-control-hpp-controloperationresult"></a>
### `ControlOperationResult`

```cpp
struct ControlOperationResult;
```

<a id="libtmux-control-hpp-controloperationresult-attribution"></a>
#### `ControlOperationResult::attribution`

```cpp
Attribution attribution{Attribution::unknown};
```

<a id="libtmux-control-hpp-controloperationresult-block"></a>
#### `ControlOperationResult::block`

```cpp
std::optional<ControlBlock> block;
```

<a id="libtmux-control-hpp-controlrequestresult"></a>
### `ControlRequestResult`

```cpp
struct ControlRequestResult;
```

<a id="libtmux-control-hpp-controlrequestresult-operations"></a>
#### `ControlRequestResult::operations`

```cpp
std::vector<ControlOperationResult> operations;
```

<a id="libtmux-control-hpp-controlrequestresult-connection-error"></a>
#### `ControlRequestResult::connection_error`

```cpp
std::optional<ProtocolError> connection_error;
```

<a id="libtmux-control-hpp-connectionoptions"></a>
### `ConnectionOptions`

```cpp
struct ConnectionOptions;
```

<a id="libtmux-control-hpp-connectionoptions-tmux-binary"></a>
#### `ConnectionOptions::tmux_binary`

```cpp
std::filesystem::path tmux_binary{"tmux"};
```

<a id="libtmux-control-hpp-connectionoptions-socket-path"></a>
#### `ConnectionOptions::socket_path`

```cpp
std::filesystem::path socket_path{};
```

<a id="libtmux-control-hpp-connectionoptions-session-name"></a>
#### `ConnectionOptions::session_name`

```cpp
std::string session_name{};
```

<a id="libtmux-control-hpp-connectionoptions-startup-timeout"></a>
#### `ConnectionOptions::startup_timeout`

```cpp
std::chrono::milliseconds startup_timeout{2000};
```

<a id="libtmux-control-hpp-connectionoptions-shutdown-timeout"></a>
#### `ConnectionOptions::shutdown_timeout`

```cpp
std::chrono::milliseconds shutdown_timeout{2000};
```

<a id="libtmux-control-hpp-connectionoptions-retained-reply-bytes"></a>
#### `ConnectionOptions::retained_reply_bytes`

```cpp
std::size_t retained_reply_bytes{kDefaultRetainedReplyBytes};
```
Passed to the decoder. Raise the first to hold a bigger capture; the second bounds a line that never ends and wants raising only if tmux grows a longer one.

<a id="libtmux-control-hpp-connectionoptions-line-bytes"></a>
#### `ConnectionOptions::line_bytes`

```cpp
std::size_t line_bytes{kDefaultLineBytes};
```

<a id="libtmux-control-hpp-connectionoptions-pane-output"></a>
#### `ConnectionOptions::pane_output`

```cpp
bool pane_output{false};
```
Deliver `%output` for every pane, as notifications.  Off, so tmux is not asked to buffer pane output for a caller who never reads it. It is fixed at connect time because tmux fixes it: a connection started without output cannot be made to listen later, so this cannot be a subscription. See `docs/design/pane-output-streaming.md`.

<a id="libtmux-control-hpp-connectionoptions-pause-after"></a>
#### `ConnectionOptions::pause_after`

```cpp
std::optional<std::chrono::seconds> pause_after{};
```
Discard a pane's queued output once it is this far behind, and say so with `%pause`.  A data-loss policy rather than backpressure, and unset is a policy too: tmux then buffers until a queued block is five minutes old and closes the connection with `too far behind`. Set this and a slow reader survives having lost output; leave it and a slow enough reader loses the connection. Only meaningful with `pane_output`.  `%pause` is the sole report that anything was dropped, and it names the pane. A caller that sets this and ignores notifications has chosen to lose output silently.

<a id="libtmux-control-hpp-notificationrange"></a>
### `NotificationRange`

Everything tmux says, until the deadline, as one loop.  Draining by hand is two nested loops and a break: ask for a batch, stop if it is empty, walk it, ask again. That shape was written six times across this repository's own tests and examples before this existed, which is the argument for it.  An input range, single pass. A `ParsedNotification` views the notification it was read from, and this owns that notification only until the iterator advances — so copy what you need out of one before asking for the next.

```cpp
class NotificationRange final;
```

<a id="libtmux-control-hpp-notificationrange-notificationrange"></a>
#### `NotificationRange::NotificationRange`

```cpp
NotificationRange(Connection& connection, std::chrono::steady_clock::time_point deadline) noexcept;
```

<a id="libtmux-control-hpp-notificationrange-begin"></a>
#### `NotificationRange::begin`

```cpp
[[nodiscard]] iterator begin();
```

<a id="libtmux-control-hpp-notificationrange-end"></a>
#### `NotificationRange::end`

```cpp
[[nodiscard]] std::default_sentinel_t end() const noexcept;
```

<a id="libtmux-control-hpp-notificationrange-iterator"></a>
### `NotificationRange::iterator`

```cpp
class iterator final;
```

<a id="libtmux-control-hpp-notificationrange-iterator-difference-type"></a>
#### `NotificationRange::iterator::difference_type`

```cpp
using difference_type = std::ptrdiff_t;
```

<a id="libtmux-control-hpp-notificationrange-iterator-value-type"></a>
#### `NotificationRange::iterator::value_type`

```cpp
using value_type = ParsedNotification;
```

<a id="libtmux-control-hpp-notificationrange-iterator-iterator-concept"></a>
#### `NotificationRange::iterator::iterator_concept`

```cpp
using iterator_concept = std::input_iterator_tag;
```

<a id="libtmux-control-hpp-notificationrange-iterator-iterator"></a>
#### `NotificationRange::iterator::iterator`

```cpp
iterator() = default;
```

<a id="libtmux-control-hpp-notificationrange-iterator-iterator-2"></a>
#### `NotificationRange::iterator::iterator`

```cpp
explicit iterator(NotificationRange* range);
```

<a id="libtmux-control-hpp-notificationrange-iterator-operator"></a>
#### `NotificationRange::iterator::operator*`

```cpp
[[nodiscard]] const ParsedNotification& operator*() const noexcept;
```

<a id="libtmux-control-hpp-notificationrange-iterator-operator-2"></a>
#### `NotificationRange::iterator::operator++`

```cpp
iterator& operator++();
```

<a id="libtmux-control-hpp-notificationrange-iterator-operator-3"></a>
#### `NotificationRange::iterator::operator++`

```cpp
void operator++(int);
```

<a id="libtmux-control-hpp-notificationrange-iterator-operator-4"></a>
#### `NotificationRange::iterator::operator==`

```cpp
[[nodiscard]] bool operator==(std::default_sentinel_t) const noexcept;
```

<a id="libtmux-control-hpp-connection"></a>
### `Connection`

```cpp
class Connection final;
```

<a id="libtmux-control-hpp-connection-connect"></a>
#### `Connection::connect`

```cpp
static expected<Connection, ProtocolError> connect(ConnectionOptions options);
```

<a id="libtmux-control-hpp-connection-connection"></a>
#### `Connection::~Connection`

```cpp
~Connection() noexcept;
```

<a id="libtmux-control-hpp-connection-connection-2"></a>
#### `Connection::Connection`

```cpp
Connection(Connection&&) noexcept;
```

<a id="libtmux-control-hpp-connection-operator"></a>
#### `Connection::operator=`

```cpp
Connection& operator=(Connection&&) noexcept;
```

<a id="libtmux-control-hpp-connection-connection-3"></a>
#### `Connection::Connection`

```cpp
Connection(const Connection&) = delete;
```

<a id="libtmux-control-hpp-connection-operator-2"></a>
#### `Connection::operator=`

```cpp
Connection& operator=(const Connection&) = delete;
```

<a id="libtmux-control-hpp-connection-execute"></a>
#### `Connection::execute`

```cpp
ControlRequestResult execute(ControlRequest request, std::chrono::steady_clock::time_point deadline);
```

<a id="libtmux-control-hpp-connection-take-notifications"></a>
#### `Connection::take_notifications`

```cpp
[[nodiscard]] std::vector<Notification> take_notifications();
```
Everything tmux has said since the last call, and how many were dropped to keep the buffer bounded.

<a id="libtmux-control-hpp-connection-wait-for-notifications"></a>
#### `Connection::wait_for_notifications`

```cpp
[[nodiscard]] std::vector<Notification> wait_for_notifications(std::chrono::steady_clock::time_point deadline);
```
The same, but waits for something to arrive.  `take_notifications` returns immediately, so a caller reacting to tmux had to call it in a loop and sleep between — which either wakes too often or reacts too late, and picks that trade with no idea how long the next event will take. This blocks until at least one notification is available, the connection fails, or the deadline passes, and returns whatever it has.  An empty result means the deadline passed or the stream ended; the two are told apart by asking `execute` or `shutdown`, which report the failure. Notifications already buffered are returned without waiting at all.

<a id="libtmux-control-hpp-connection-notification-fd"></a>
#### `Connection::notification_fd`

```cpp
[[nodiscard]] int notification_fd() const noexcept;
```
A descriptor that is readable exactly when a take would return something.  For a caller who owns their own event loop. Without it, integrating means a thread blocked in `wait_for_notifications`, a queue of their own, and a self-pipe to wake the loop — which is this descriptor, rebuilt by hand on top of the thread and queue this connection already has.  Do not read from it: readability is the signal and the byte is this connection's to consume. Drain with `take_notifications`, which clears it. A broken stream makes it readable too, so a poller learns of the failure rather than waiting for an answer that cannot come.  Valid until the connection is destroyed or moved from; `-1` if the pipe could not be created.

<a id="libtmux-control-hpp-connection-set-pane-output"></a>
#### `Connection::set_pane_output`

```cpp
expected<void, ProtocolError> set_pane_output(std::string_view pane, bool deliver, std::chrono::steady_clock::time_point deadline);
```
Stop or resume `%output` for one pane, on a connection that asked for it.  The direction is not symmetrical, because tmux is not: a connection that started without `pane_output` cannot be made to listen to anything, and muting is the only per-pane control it offers. So this narrows what a listening connection receives; it cannot widen a silent one.  `resume` on a pane that tmux paused also clears the pause, and tmux moves that pane's offset to the current end — so whatever was produced while it was paused or muted is not delivered afterwards.

<a id="libtmux-control-hpp-connection-events"></a>
#### `Connection::events`

```cpp
[[nodiscard]] NotificationRange events(std::chrono::steady_clock::time_point deadline);
```
Everything tmux says until the deadline, as one loop rather than two.  Borrows this connection, which must outlive it.

<a id="libtmux-control-hpp-connection-dropped-notifications"></a>
#### `Connection::dropped_notifications`

```cpp
[[nodiscard]] std::size_t dropped_notifications() const noexcept;
```

<a id="libtmux-control-hpp-connection-native-child-pid"></a>
#### `Connection::native_child_pid`

```cpp
[[nodiscard]] std::int64_t native_child_pid() const noexcept;
```

<a id="libtmux-control-hpp-connection-shutdown"></a>
#### `Connection::shutdown`

```cpp
expected<void, ProtocolError> shutdown(std::chrono::steady_clock::time_point deadline);
```

<a id="libtmux-control-hpp-free-symbols"></a>
### `Free symbols`

<a id="libtmux-control-hpp-free-symbols-kdefaultretainedreplybytes"></a>
#### `kDefaultRetainedReplyBytes`

```cpp
inline constexpr std::size_t kDefaultRetainedReplyBytes = 1024U * 1024U;
```
How much of one reply a decoder holds, and how long a single line may grow before the stream is called broken.  A subprocess ends and gives its memory back; a connection does not, so the bound has to be in the decoder rather than in whatever reads it afterwards. The reply bound is the subprocess transport's capture limit, so the same call costs the same memory over either transport. The line bound has no subprocess equivalent: it is the point past which an unterminated line is evidence of a broken stream rather than a large answer.

<a id="libtmux-control-hpp-free-symbols-kdefaultlinebytes"></a>
#### `kDefaultLineBytes`

```cpp
inline constexpr std::size_t kDefaultLineBytes = 1024U * 1024U;
```

<a id="libtmux-control-hpp-free-symbols-event"></a>
#### `Event`

```cpp
using Event = std::variant<ControlBlock, Notification>;
```

<a id="libtmux-control-hpp-free-symbols-to-string"></a>
#### `to_string`

```cpp
[[nodiscard]] std::string_view to_string(NotificationKind kind) noexcept;
```

<a id="libtmux-control-hpp-free-symbols-parse"></a>
#### `parse`

```cpp
[[nodiscard]] ParsedNotification parse(const Notification& notification);
```

<a id="libtmux-control-hpp-free-symbols-parse-2"></a>
#### `parse`

```cpp
ParsedNotification parse(Notification&&) = delete;
```

<a id="libtmux-batch-hpp"></a>
## `libtmux/batch.hpp`

Build one tmux command sequence from several commands.  tmux accepts multiple commands in a single invocation separated by a `;` argument. Because the core execs argv directly and never goes through a shell, the separator is a bare `;` element — there is no backslash to escape and no quoting to get wrong.  A batch is one fail-fast group: tmux stops at the first command that errors. That is why a batch is a distinct type from a list of independent requests, which the transport runs separately and attributes individually.

**Symbols:**

- [`CommandBatch`](#libtmux-batch-hpp-commandbatch)
  - [`CommandBatch::add`](#libtmux-batch-hpp-commandbatch-add)
  - [`CommandBatch::size`](#libtmux-batch-hpp-commandbatch-size)
  - [`CommandBatch::empty`](#libtmux-batch-hpp-commandbatch-empty)
  - [`CommandBatch::argv`](#libtmux-batch-hpp-commandbatch-argv)
  - [`CommandBatch::commands`](#libtmux-batch-hpp-commandbatch-commands)
- [`Free symbols`](#libtmux-batch-hpp-free-symbols)
  - [`kCommandSeparator`](#libtmux-batch-hpp-free-symbols-kcommandseparator)

<a id="libtmux-batch-hpp-commandbatch"></a>
### `CommandBatch`

```cpp
class CommandBatch;
```

<a id="libtmux-batch-hpp-commandbatch-add"></a>
#### `CommandBatch::add`

```cpp
bool add(std::vector<std::string> command);
```
Append one command. An empty command is rejected rather than emitted, because an empty argv between separators makes tmux read the next command's name as an argument.

<a id="libtmux-batch-hpp-commandbatch-size"></a>
#### `CommandBatch::size`

```cpp
[[nodiscard]] std::size_t size() const noexcept;
```

<a id="libtmux-batch-hpp-commandbatch-empty"></a>
#### `CommandBatch::empty`

```cpp
[[nodiscard]] bool empty() const noexcept;
```

<a id="libtmux-batch-hpp-commandbatch-argv"></a>
#### `CommandBatch::argv`

```cpp
[[nodiscard]] std::vector<std::string> argv() const;
```
Render the whole batch as one argv. A single command renders with no separator, so a batch of one is byte-identical to running it alone.

<a id="libtmux-batch-hpp-commandbatch-commands"></a>
#### `CommandBatch::commands`

```cpp
[[nodiscard]] const std::vector<std::vector<std::string>>& commands() const noexcept;
```

<a id="libtmux-batch-hpp-free-symbols"></a>
### `Free symbols`

<a id="libtmux-batch-hpp-free-symbols-kcommandseparator"></a>
#### `kCommandSeparator`

```cpp
inline constexpr std::string_view kCommandSeparator = ";";
```

<a id="libtmux-chain-hpp"></a>
## `libtmux/chain.hpp`

Compose several tmux commands as one fail-fast group.  A chain is a typed front for a batch: each step validates its own arguments as it is added, so a bad target or key name is reported where it was written rather than as a tmux message about a command the caller cannot see.  The chain records the first validation failure and stops accumulating. That keeps the fluent form honest — the alternative, throwing mid-expression or silently dropping a step, both leave the caller guessing which parts ran.

**Symbols:**

- [`Chain`](#libtmux-chain-hpp-chain)
  - [`Chain::new_session`](#libtmux-chain-hpp-chain-new-session)
  - [`Chain::new_window`](#libtmux-chain-hpp-chain-new-window)
  - [`Chain::split_window`](#libtmux-chain-hpp-chain-split-window)
  - [`Chain::send_text`](#libtmux-chain-hpp-chain-send-text)
  - [`Chain::send_key`](#libtmux-chain-hpp-chain-send-key)
  - [`Chain::command`](#libtmux-chain-hpp-chain-command)
  - [`Chain::valid`](#libtmux-chain-hpp-chain-valid)
  - [`Chain::error`](#libtmux-chain-hpp-chain-error)
  - [`Chain::batch`](#libtmux-chain-hpp-chain-batch)

<a id="libtmux-chain-hpp-chain"></a>
### `Chain`

```cpp
class Chain;
```

<a id="libtmux-chain-hpp-chain-new-session"></a>
#### `Chain::new_session`

```cpp
Chain& new_session(std::string_view name, bool detached = true);
```

<a id="libtmux-chain-hpp-chain-new-window"></a>
#### `Chain::new_window`

```cpp
Chain& new_window(std::string_view session, std::string_view name);
```

<a id="libtmux-chain-hpp-chain-split-window"></a>
#### `Chain::split_window`

```cpp
Chain& split_window(std::string_view session, std::string_view window);
```

<a id="libtmux-chain-hpp-chain-send-text"></a>
#### `Chain::send_text`

```cpp
Chain& send_text(std::string_view target, std::string_view text);
```
Literal text, never interpreted as key names or formats.

<a id="libtmux-chain-hpp-chain-send-key"></a>
#### `Chain::send_key`

```cpp
Chain& send_key(std::string_view target, std::string_view key);
```

<a id="libtmux-chain-hpp-chain-command"></a>
#### `Chain::command`

```cpp
Chain& command(std::vector<std::string> argv);
```
Escape hatch for a command the typed steps do not cover.

<a id="libtmux-chain-hpp-chain-valid"></a>
#### `Chain::valid`

```cpp
[[nodiscard]] bool valid() const noexcept;
```

<a id="libtmux-chain-hpp-chain-error"></a>
#### `Chain::error`

```cpp
[[nodiscard]] const std::string& error() const noexcept;
```

<a id="libtmux-chain-hpp-chain-batch"></a>
#### `Chain::batch`

```cpp
[[nodiscard]] const CommandBatch& batch() const noexcept;
```

<a id="libtmux-keys-hpp"></a>
## `libtmux/keys.hpp`

Build `send-keys` arguments.  tmux does not report an unknown key name: `send-keys NoSuchKey` succeeds silently, so a typo is invisible at the call site and shows up later as input that never arrived. Key names are therefore validated here, where the caller can still be told.  Literal text takes `-l`, under which tmux interprets nothing — not key names, not formats. Choosing between the two is the caller's decision and is never inferred from the text.

**Symbols:**

- [`KeyError`](#libtmux-keys-hpp-keyerror)
  - [`KeyError::empty`](#libtmux-keys-hpp-keyerror-empty)
  - [`KeyError::unknown_name`](#libtmux-keys-hpp-keyerror-unknown-name)
- [`Free symbols`](#libtmux-keys-hpp-free-symbols)
  - [`to_string`](#libtmux-keys-hpp-free-symbols-to-string)
  - [`kNamedKeys`](#libtmux-keys-hpp-free-symbols-knamedkeys)
  - [`is_function_key`](#libtmux-keys-hpp-free-symbols-is-function-key)
  - [`is_key_name`](#libtmux-keys-hpp-free-symbols-is-key-name)
  - [`literal_arguments`](#libtmux-keys-hpp-free-symbols-literal-arguments)

<a id="libtmux-keys-hpp-keyerror"></a>
### `KeyError`

```cpp
enum class KeyError;
```

<a id="libtmux-keys-hpp-keyerror-empty"></a>
#### `KeyError::empty` — `empty,`

<a id="libtmux-keys-hpp-keyerror-unknown-name"></a>
#### `KeyError::unknown_name` — `unknown_name,`

<a id="libtmux-keys-hpp-free-symbols"></a>
### `Free symbols`

<a id="libtmux-keys-hpp-free-symbols-to-string"></a>
#### `to_string`

```cpp
[[nodiscard]] constexpr std::string_view to_string(KeyError error) noexcept;
```

<a id="libtmux-keys-hpp-free-symbols-knamedkeys"></a>
#### `kNamedKeys`

```cpp
inline constexpr std::array kNamedKeys{ std::string_view{"BSpace"}, std::string_view{"BTab"}, std::string_view{"DC"}, std::string_view{"Delete"}, std::string_view{"Down"}, std::string_view{"End"}, std::string_view{"Enter"}, std::string_view{"Escape"}, std::string_view{"Home"}, std::string_view{"IC"}, std::string_view{"Insert"}, std::string_view{"KP*"}, std::string_view{"KP+"}, std::string_view{"KP-"}, std::string_view{"KP."}, std::string_view{"KP/"}, std::string_view{"KP0"}, std::string_view{"KP1"}, std::string_view{"KP2"}, std::string_view{"KP3"}, std::string_view{"KP4"}, std::string_view{"KP5"}, std::string_view{"KP6"}, std::string_view{"KP7"}, std::string_view{"KP8"}, std::string_view{"KP9"}, std::string_view{"KPEnter"}, std::string_view{"Left"}, std::string_view{"NPage"}, std::string_view{"PPage"}, std::string_view{"PageDown"}, std::string_view{"PageUp"}, std::string_view{"PgDn"}, std::string_view{"PgUp"}, std::string_view{"Right"}, std::string_view{"Space"}, std::string_view{"Tab"}, std::string_view{"Up"}, };
```
Named keys tmux accepts, taken from the table in its own key-string.c at the oldest supported release. Function keys are generated rather than listed, and a single character is handled separately.

<a id="libtmux-keys-hpp-free-symbols-is-function-key"></a>
#### `is_function_key`

```cpp
[[nodiscard]] inline bool is_function_key(std::string_view name) noexcept;
```

<a id="libtmux-keys-hpp-free-symbols-is-key-name"></a>
#### `is_key_name`

```cpp
[[nodiscard]] inline bool is_key_name(std::string_view key) noexcept;
```
Accept a key with any number of C-, M-, or S- modifiers.

<a id="libtmux-keys-hpp-free-symbols-literal-arguments"></a>
#### `literal_arguments`

```cpp
[[nodiscard]] inline expected<std::vector<std::string>, KeyError> literal_arguments(std::string_view text);
```
Send text exactly as written: the flag, the end of flags, and the text.  `--` is part of the fragment rather than something a caller appends, because text beginning with a dash is read as another `send-keys` option without it and that is not a mistake worth making twice. It was made twice: this returned the flag and the text alone, `Pane::send_text` inserted the separator afterwards, and `Chain::send_text` did not — so the same text through the two spellings reached tmux as two different commands.

<a id="libtmux-capture-hpp"></a>
## `libtmux/capture.hpp`

Split `capture-pane -p` output into lines.  Every captured line is newline-terminated, so a naive split on '\n' yields a final empty element that is not a line. Blank rows inside the pane are genuine empty lines and must survive, which is why the terminator is removed by dropping exactly one trailing element rather than by trimming empties.  `-p` already strips trailing whitespace from each line; `-N` preserves it. Neither is re-implemented here: the caller chooses the flag and this only frames what tmux returned.

**Symbols:**

- [`Free symbols`](#libtmux-capture-hpp-free-symbols)
  - [`capture_lines`](#libtmux-capture-hpp-free-symbols-capture-lines)
  - [`capture_lines`](#libtmux-capture-hpp-free-symbols-capture-lines-2)
  - [`without_trailing_blanks`](#libtmux-capture-hpp-free-symbols-without-trailing-blanks)

<a id="libtmux-capture-hpp-free-symbols"></a>
### `Free symbols`

<a id="libtmux-capture-hpp-free-symbols-capture-lines"></a>
#### `capture_lines`

```cpp
template <typename Text> requires std::same_as<std::remove_cvref_t<Text>, std::string> && (!std::is_lvalue_reference_v<Text>) std::vector<std::string_view> capture_lines(Text&&) = delete;
```
The lines are views into the text, so text that dies at the semicolon takes them with it — and `pane.capture()` returns its output by value, which makes `capture_lines(*pane.capture())` the natural thing to write and a use-after-free to run. Deleted for an rvalue string only: an lvalue string converts as before, and so does a literal.

<a id="libtmux-capture-hpp-free-symbols-capture-lines-2"></a>
#### `capture_lines`

```cpp
[[nodiscard]] inline std::vector<std::string_view> capture_lines(std::string_view output);
```

<a id="libtmux-capture-hpp-free-symbols-without-trailing-blanks"></a>
#### `without_trailing_blanks`

```cpp
[[nodiscard]] inline std::vector<std::string_view> without_trailing_blanks(std::vector<std::string_view> lines);
```
Drop the blank rows a pane pads its height with, keeping blank lines that have content below them.

<a id="libtmux-target-hpp"></a>
## `libtmux/target.hpp`

Build tmux target specifiers.  tmux addresses objects either by id (`$0`, `@0`, `%0`) or by the `session:window.pane` path. Ids are unambiguous; the path is not, because `:` and `.` are its separators and a session or window name containing one silently re-parses as a different target. Prefer an id whenever one exists, and refuse to build a path from a name that cannot survive it.

**Symbols:**

- [`TargetError`](#libtmux-target-hpp-targeterror)
  - [`TargetError::empty_name`](#libtmux-target-hpp-targeterror-empty-name)
  - [`TargetError::separator_in_name`](#libtmux-target-hpp-targeterror-separator-in-name)
- [`Free symbols`](#libtmux-target-hpp-free-symbols)
  - [`to_string`](#libtmux-target-hpp-free-symbols-to-string)
  - [`is_pane_id`](#libtmux-target-hpp-free-symbols-is-pane-id)
  - [`is_window_id`](#libtmux-target-hpp-free-symbols-is-window-id)
  - [`is_session_id`](#libtmux-target-hpp-free-symbols-is-session-id)
  - [`path_component`](#libtmux-target-hpp-free-symbols-path-component)
  - [`session_target`](#libtmux-target-hpp-free-symbols-session-target)
  - [`window_target`](#libtmux-target-hpp-free-symbols-window-target)
  - [`pane_target`](#libtmux-target-hpp-free-symbols-pane-target)

<a id="libtmux-target-hpp-targeterror"></a>
### `TargetError`

```cpp
enum class TargetError;
```

<a id="libtmux-target-hpp-targeterror-empty-name"></a>
#### `TargetError::empty_name` — `empty_name,`

<a id="libtmux-target-hpp-targeterror-separator-in-name"></a>
#### `TargetError::separator_in_name` — `separator_in_name,`

A name holding a separator would re-parse as a different target.

<a id="libtmux-target-hpp-free-symbols"></a>
### `Free symbols`

<a id="libtmux-target-hpp-free-symbols-to-string"></a>
#### `to_string`

```cpp
[[nodiscard]] constexpr std::string_view to_string(TargetError error) noexcept;
```

<a id="libtmux-target-hpp-free-symbols-is-pane-id"></a>
#### `is_pane_id`

```cpp
[[nodiscard]] constexpr bool is_pane_id(std::string_view value) noexcept;
```
Whether a value is written as an id, which is what decides that it needs no separator validation below — not whether tmux would resolve it. `%x` is an id by this test and no pane by tmux's, and both are right: it carries no separator, so composing it can only produce the caller's own mistake back.

<a id="libtmux-target-hpp-free-symbols-is-window-id"></a>
#### `is_window_id`

```cpp
[[nodiscard]] constexpr bool is_window_id(std::string_view value) noexcept;
```

<a id="libtmux-target-hpp-free-symbols-is-session-id"></a>
#### `is_session_id`

```cpp
[[nodiscard]] constexpr bool is_session_id(std::string_view value) noexcept;
```

<a id="libtmux-target-hpp-free-symbols-path-component"></a>
#### `path_component`

```cpp
[[nodiscard]] inline expected<std::string, TargetError> path_component(std::string_view name);
```
Validate one path component. Ids skip validation because they contain no separator by construction.

<a id="libtmux-target-hpp-free-symbols-session-target"></a>
#### `session_target`

```cpp
[[nodiscard]] inline expected<std::string, TargetError> session_target(std::string_view session);
```
A session target is its id, or its validated name.

<a id="libtmux-target-hpp-free-symbols-window-target"></a>
#### `window_target`

```cpp
[[nodiscard]] inline expected<std::string, TargetError> window_target(std::string_view session, std::string_view window);
```
A window target is its id, which needs no session, or `session:window`.

<a id="libtmux-target-hpp-free-symbols-pane-target"></a>
#### `pane_target`

```cpp
[[nodiscard]] inline expected<std::string, TargetError> pane_target(std::string_view session, std::string_view window, std::string_view pane);
```
A pane target is its id, or `session:window.pane`.

<a id="libtmux-socket-hpp"></a>
## `libtmux/socket.hpp`

Build the connection arguments that select a tmux-compatible server.  tmux selects a server either by socket name (`-L`) or by socket path (`-S`). A name is resolved under the socket directory, so it must be a single path component; a path is used verbatim. Either way the result must fit a UNIX domain socket address. Windows psmux supports names but has no `-S` transport, so socket paths are rejected there.

**Symbols:**

- [`SocketError`](#libtmux-socket-hpp-socketerror)
  - [`SocketError::empty`](#libtmux-socket-hpp-socketerror-empty)
  - [`SocketError::name_has_separator`](#libtmux-socket-hpp-socketerror-name-has-separator)
  - [`SocketError::path_too_long`](#libtmux-socket-hpp-socketerror-path-too-long)
  - [`SocketError::path_unsupported`](#libtmux-socket-hpp-socketerror-path-unsupported)
- [`Free symbols`](#libtmux-socket-hpp-free-symbols)
  - [`kSocketPathLimit`](#libtmux-socket-hpp-free-symbols-ksocketpathlimit)
  - [`kSocketPathLimit`](#libtmux-socket-hpp-free-symbols-ksocketpathlimit-2)
  - [`to_string`](#libtmux-socket-hpp-free-symbols-to-string)
  - [`socket_name_arguments`](#libtmux-socket-hpp-free-symbols-socket-name-arguments)
  - [`socket_path_arguments`](#libtmux-socket-hpp-free-symbols-socket-path-arguments)

<a id="libtmux-socket-hpp-socketerror"></a>
### `SocketError`

```cpp
enum class SocketError;
```

<a id="libtmux-socket-hpp-socketerror-empty"></a>
#### `SocketError::empty` — `empty,`

<a id="libtmux-socket-hpp-socketerror-name-has-separator"></a>
#### `SocketError::name_has_separator` — `name_has_separator,`

<a id="libtmux-socket-hpp-socketerror-path-too-long"></a>
#### `SocketError::path_too_long` — `path_too_long,`

<a id="libtmux-socket-hpp-socketerror-path-unsupported"></a>
#### `SocketError::path_unsupported` — `path_unsupported,`

<a id="libtmux-socket-hpp-free-symbols"></a>
### `Free symbols`

<a id="libtmux-socket-hpp-free-symbols-ksocketpathlimit"></a>
#### `kSocketPathLimit`

```cpp
inline constexpr std::size_t kSocketPathLimit = 0U;
```
Available when `defined(_WIN32)`.

<a id="libtmux-socket-hpp-free-symbols-ksocketpathlimit-2"></a>
#### `kSocketPathLimit`

```cpp
inline constexpr std::size_t kSocketPathLimit = sizeof(sockaddr_un::sun_path) - 1U;
```
Available when `!(defined(_WIN32))`.

<a id="libtmux-socket-hpp-free-symbols-to-string"></a>
#### `to_string`

```cpp
[[nodiscard]] constexpr std::string_view to_string(SocketError error) noexcept;
```

<a id="libtmux-socket-hpp-free-symbols-socket-name-arguments"></a>
#### `socket_name_arguments`

```cpp
[[nodiscard]] inline expected<std::vector<std::string>, SocketError> socket_name_arguments(std::string_view name);
```
`-L name`: a single component resolved under the socket directory.

<a id="libtmux-socket-hpp-free-symbols-socket-path-arguments"></a>
#### `socket_path_arguments`

```cpp
[[nodiscard]] inline expected<std::vector<std::string>, SocketError> socket_path_arguments(std::string_view path);
```
`-S path`: used verbatim, so the address limit applies to it directly.

<a id="libtmux-format-hpp"></a>
## `libtmux/format.hpp`

Compose tmux format strings.  tmux treats `#` as the start of a substitution: `#{...}` expands a variable, `#(...)` runs a command, and `##` is a literal `#`. Any literal text placed in a format must therefore escape its `#` characters, or a status line containing `#1` silently becomes an expansion attempt.

**Symbols:**

- [`FormatBuilder`](#libtmux-format-hpp-formatbuilder)
  - [`FormatBuilder::literal`](#libtmux-format-hpp-formatbuilder-literal)
  - [`FormatBuilder::field`](#libtmux-format-hpp-formatbuilder-field)
  - [`FormatBuilder::str`](#libtmux-format-hpp-formatbuilder-str)
- [`Free symbols`](#libtmux-format-hpp-free-symbols)
  - [`escape_literal`](#libtmux-format-hpp-free-symbols-escape-literal)
  - [`variable`](#libtmux-format-hpp-free-symbols-variable)

<a id="libtmux-format-hpp-formatbuilder"></a>
### `FormatBuilder`

Build a format from alternating literal and variable pieces so a caller never hand-concatenates and forgets to escape one of them.

```cpp
class FormatBuilder;
```

<a id="libtmux-format-hpp-formatbuilder-literal"></a>
#### `FormatBuilder::literal`

```cpp
FormatBuilder& literal(std::string_view text);
```

<a id="libtmux-format-hpp-formatbuilder-field"></a>
#### `FormatBuilder::field`

```cpp
FormatBuilder& field(std::string_view name);
```

<a id="libtmux-format-hpp-formatbuilder-str"></a>
#### `FormatBuilder::str`

```cpp
[[nodiscard]] const std::string& str() const noexcept;
```

<a id="libtmux-format-hpp-free-symbols"></a>
### `Free symbols`

<a id="libtmux-format-hpp-free-symbols-escape-literal"></a>
#### `escape_literal`

```cpp
[[nodiscard]] inline std::string escape_literal(std::string_view text);
```
Escape literal text for inclusion in a format string.

<a id="libtmux-format-hpp-free-symbols-variable"></a>
#### `variable`

```cpp
[[nodiscard]] inline std::string variable(std::string_view name);
```
Wrap a variable name as a substitution. The name is not escaped: a name is chosen by the caller from tmux's format vocabulary, not built from data.

<a id="libtmux-version-hpp"></a>
## `libtmux/version.hpp`

Parse and order tmux version strings.  Suffixes follow bare releases; `next-` precedes its release and `master` follows all. Psmux 3.3.7 occupies `revision=7`, equal to tmux 3.3g.

**Symbols:**

- [`VersionError`](#libtmux-version-hpp-versionerror)
  - [`VersionError::missing_prefix`](#libtmux-version-hpp-versionerror-missing-prefix)
  - [`VersionError::malformed`](#libtmux-version-hpp-versionerror-malformed)
- [`Version`](#libtmux-version-hpp-version)
  - [`Version::major`](#libtmux-version-hpp-version-major)
  - [`Version::minor`](#libtmux-version-hpp-version-minor)
  - [`Version::revision`](#libtmux-version-hpp-version-revision)
  - [`Version::prerelease`](#libtmux-version-hpp-version-prerelease)
  - [`Version::unbounded`](#libtmux-version-hpp-version-unbounded)
  - [`Version::operator<=>`](#libtmux-version-hpp-version-operator)
  - [`Version::operator==`](#libtmux-version-hpp-version-operator-2)
- [`Free symbols`](#libtmux-version-hpp-free-symbols)
  - [`to_string`](#libtmux-version-hpp-free-symbols-to-string)
  - [`parse_version`](#libtmux-version-hpp-free-symbols-parse-version)
  - [`kMinimumSupported`](#libtmux-version-hpp-free-symbols-kminimumsupported)
  - [`is_supported`](#libtmux-version-hpp-free-symbols-is-supported)
  - [`library_version`](#libtmux-version-hpp-free-symbols-library-version)

<a id="libtmux-version-hpp-versionerror"></a>
### `VersionError`

```cpp
enum class VersionError;
```

<a id="libtmux-version-hpp-versionerror-missing-prefix"></a>
#### `VersionError::missing_prefix` — `missing_prefix,`

<a id="libtmux-version-hpp-versionerror-malformed"></a>
#### `VersionError::malformed` — `malformed,`

<a id="libtmux-version-hpp-version"></a>
### `Version`

```cpp
struct Version;
```

<a id="libtmux-version-hpp-version-major"></a>
#### `Version::major`

```cpp
std::uint32_t major{};
```

<a id="libtmux-version-hpp-version-minor"></a>
#### `Version::minor`

```cpp
std::uint32_t minor{};
```

<a id="libtmux-version-hpp-version-revision"></a>
#### `Version::revision`

```cpp
std::uint32_t revision{};
```
0 for a bare release, 1 for `a`, 2 for `b`, and so on. Psmux's numeric third component occupies this existing slot as the corresponding number.

<a id="libtmux-version-hpp-version-prerelease"></a>
#### `Version::prerelease`

```cpp
bool prerelease{false};
```
A `next-` build precedes the release it leads to; `master` follows every numbered release.

<a id="libtmux-version-hpp-version-unbounded"></a>
#### `Version::unbounded`

```cpp
bool unbounded{false};
```

<a id="libtmux-version-hpp-version-operator"></a>
#### `Version::operator<=>`

```cpp
[[nodiscard]] constexpr std::strong_ordering operator<=>(const Version& other) const noexcept;
```

<a id="libtmux-version-hpp-version-operator-2"></a>
#### `Version::operator==`

```cpp
[[nodiscard]] constexpr bool operator==(const Version&) const noexcept = default;
```

<a id="libtmux-version-hpp-free-symbols"></a>
### `Free symbols`

<a id="libtmux-version-hpp-free-symbols-to-string"></a>
#### `to_string`

```cpp
[[nodiscard]] constexpr std::string_view to_string(VersionError error) noexcept;
```

<a id="libtmux-version-hpp-free-symbols-parse-version"></a>
#### `parse_version`

```cpp
[[nodiscard]] inline expected<Version, VersionError> parse_version(std::string_view output);
```
Parse the first line of `tmux -V`, with or without its trailing newline.

<a id="libtmux-version-hpp-free-symbols-kminimumsupported"></a>
#### `kMinimumSupported`

```cpp
inline constexpr Version kMinimumSupported{.major = 3, .minor = 2, .revision = 1};
```
The oldest release this library supports, matching the Python package.

<a id="libtmux-version-hpp-free-symbols-is-supported"></a>
#### `is_supported`

```cpp
[[nodiscard]] constexpr bool is_supported(const Version& version) noexcept;
```

<a id="libtmux-version-hpp-free-symbols-library-version"></a>
#### `library_version`

```cpp
[[nodiscard]] std::string_view library_version() noexcept;
```
This package's own version, not tmux's.

<a id="libtmux-lowering-hpp"></a>
## `libtmux/lowering.hpp`

Walk a FilterExpr and hand its shape to a caller-supplied sink.  The core stays dependency-free by never naming a serialization library: the sink is whatever the caller passes, so a JSON integration lives entirely outside this header. The same walk is what a future tmux `-f` compiler will use, which is why the node keeps its field name rather than only its accessor.

**Symbols:**

- [`Free symbols`](#libtmux-lowering-hpp-free-symbols)
  - [`name_of`](#libtmux-lowering-hpp-free-symbols-name-of)
  - [`name_of`](#libtmux-lowering-hpp-free-symbols-name-of-2)
  - [`replay`](#libtmux-lowering-hpp-free-symbols-replay)
  - [`lower`](#libtmux-lowering-hpp-free-symbols-lower)

<a id="libtmux-lowering-hpp-free-symbols"></a>
### `Free symbols`

<a id="libtmux-lowering-hpp-free-symbols-name-of"></a>
#### `name_of`

```cpp
[[nodiscard]] constexpr std::string_view name_of(StringOp op) noexcept;
```

<a id="libtmux-lowering-hpp-free-symbols-name-of-2"></a>
#### `name_of`

```cpp
[[nodiscard]] constexpr std::string_view name_of(NumberOp op) noexcept;
```

<a id="libtmux-lowering-hpp-free-symbols-replay"></a>
#### `replay`

```cpp
template <typename Sink> void replay(const LoweredExpression& nodes, Sink& sink);
```
Replay an already-lowered child into a sink.

<a id="libtmux-lowering-hpp-free-symbols-lower"></a>
#### `lower`

```cpp
template <typename Entity, typename Sink> void lower(const FilterExpr<Entity>& expr, Sink& sink);
```
A sink receives one call per node in prefix order. Groups and negations are bracketed by begin/end so a sink never has to count operands itself.

<a id="libtmux-lowered-node-hpp"></a>
## `libtmux/lowered_node.hpp`

One node of a lowered expression.  Lowering flattens an expression into a sequence a caller can serialize or compile without knowing the entity type it came from. That erasure is what lets a relation keep its child: the child compares a different entity, so it cannot live in the parent's variant, but its lowered form can.

**Symbols:**

- [`LoweredNode`](#libtmux-lowered-node-hpp-lowerednode)
  - [`LoweredNode::kind`](#libtmux-lowered-node-hpp-lowerednode-kind)
  - [`LoweredNode::name`](#libtmux-lowered-node-hpp-lowerednode-name)
  - [`LoweredNode::op`](#libtmux-lowered-node-hpp-lowerednode-op)
  - [`LoweredNode::operand`](#libtmux-lowered-node-hpp-lowerednode-operand)
  - [`LoweredNode::number`](#libtmux-lowered-node-hpp-lowerednode-number)
  - [`LoweredNode::conjunction`](#libtmux-lowered-node-hpp-lowerednode-conjunction)
  - [`LoweredNode::expected`](#libtmux-lowered-node-hpp-lowerednode-expected)
  - [`LoweredNode::quantifier`](#libtmux-lowered-node-hpp-lowerednode-quantifier)
- [`LoweredNode::Kind`](#libtmux-lowered-node-hpp-lowerednode-kind-2)
  - [`LoweredNode::Kind::string_test`](#libtmux-lowered-node-hpp-lowerednode-kind-string-test)
  - [`LoweredNode::Kind::bool_test`](#libtmux-lowered-node-hpp-lowerednode-kind-bool-test)
  - [`LoweredNode::Kind::number_test`](#libtmux-lowered-node-hpp-lowerednode-kind-number-test)
  - [`LoweredNode::Kind::begin_group`](#libtmux-lowered-node-hpp-lowerednode-kind-begin-group)
  - [`LoweredNode::Kind::end_group`](#libtmux-lowered-node-hpp-lowerednode-kind-end-group)
  - [`LoweredNode::Kind::begin_negation`](#libtmux-lowered-node-hpp-lowerednode-kind-begin-negation)
  - [`LoweredNode::Kind::end_negation`](#libtmux-lowered-node-hpp-lowerednode-kind-end-negation)
  - [`LoweredNode::Kind::begin_relation`](#libtmux-lowered-node-hpp-lowerednode-kind-begin-relation)
  - [`LoweredNode::Kind::end_relation`](#libtmux-lowered-node-hpp-lowerednode-kind-end-relation)
- [`NodeCollector`](#libtmux-lowered-node-hpp-nodecollector)
  - [`NodeCollector::string_test`](#libtmux-lowered-node-hpp-nodecollector-string-test)
  - [`NodeCollector::bool_test`](#libtmux-lowered-node-hpp-nodecollector-bool-test)
  - [`NodeCollector::number_test`](#libtmux-lowered-node-hpp-nodecollector-number-test)
  - [`NodeCollector::begin_group`](#libtmux-lowered-node-hpp-nodecollector-begin-group)
  - [`NodeCollector::end_group`](#libtmux-lowered-node-hpp-nodecollector-end-group)
  - [`NodeCollector::begin_negation`](#libtmux-lowered-node-hpp-nodecollector-begin-negation)
  - [`NodeCollector::end_negation`](#libtmux-lowered-node-hpp-nodecollector-end-negation)
  - [`NodeCollector::begin_relation`](#libtmux-lowered-node-hpp-nodecollector-begin-relation)
  - [`NodeCollector::end_relation`](#libtmux-lowered-node-hpp-nodecollector-end-relation)
  - [`NodeCollector::nodes`](#libtmux-lowered-node-hpp-nodecollector-nodes)
  - [`NodeCollector::take`](#libtmux-lowered-node-hpp-nodecollector-take)
- [`Free symbols`](#libtmux-lowered-node-hpp-free-symbols)
  - [`LoweredExpression`](#libtmux-lowered-node-hpp-free-symbols-loweredexpression)

<a id="libtmux-lowered-node-hpp-lowerednode"></a>
### `LoweredNode`

```cpp
struct LoweredNode;
```

<a id="libtmux-lowered-node-hpp-lowerednode-kind"></a>
#### `LoweredNode::kind`

```cpp
Kind kind{Kind::string_test};
```

<a id="libtmux-lowered-node-hpp-lowerednode-name"></a>
#### `LoweredNode::name`

```cpp
std::string name;
```
Field name for a test, relation name for a relation, empty otherwise.

<a id="libtmux-lowered-node-hpp-lowerednode-op"></a>
#### `LoweredNode::op`

```cpp
std::string op;
```

<a id="libtmux-lowered-node-hpp-lowerednode-operand"></a>
#### `LoweredNode::operand`

```cpp
std::string operand;
```

<a id="libtmux-lowered-node-hpp-lowerednode-number"></a>
#### `LoweredNode::number`

```cpp
long long number{};
```
The value a number_test compares against, kept as a number so a future tmux `-f` compiler does not have to parse it back out of the operand.

<a id="libtmux-lowered-node-hpp-lowerednode-conjunction"></a>
#### `LoweredNode::conjunction`

```cpp
bool conjunction{};
```

<a id="libtmux-lowered-node-hpp-lowerednode-expected"></a>
#### `LoweredNode::expected`

```cpp
bool expected{};
```

<a id="libtmux-lowered-node-hpp-lowerednode-quantifier"></a>
#### `LoweredNode::quantifier`

```cpp
int quantifier{};
```

<a id="libtmux-lowered-node-hpp-lowerednode-kind-2"></a>
### `LoweredNode::Kind`

```cpp
enum class Kind;
```

<a id="libtmux-lowered-node-hpp-lowerednode-kind-string-test"></a>
#### `LoweredNode::Kind::string_test` — `string_test,`

<a id="libtmux-lowered-node-hpp-lowerednode-kind-bool-test"></a>
#### `LoweredNode::Kind::bool_test` — `bool_test,`

<a id="libtmux-lowered-node-hpp-lowerednode-kind-number-test"></a>
#### `LoweredNode::Kind::number_test` — `number_test,`

<a id="libtmux-lowered-node-hpp-lowerednode-kind-begin-group"></a>
#### `LoweredNode::Kind::begin_group` — `begin_group,`

<a id="libtmux-lowered-node-hpp-lowerednode-kind-end-group"></a>
#### `LoweredNode::Kind::end_group` — `end_group,`

<a id="libtmux-lowered-node-hpp-lowerednode-kind-begin-negation"></a>
#### `LoweredNode::Kind::begin_negation` — `begin_negation,`

<a id="libtmux-lowered-node-hpp-lowerednode-kind-end-negation"></a>
#### `LoweredNode::Kind::end_negation` — `end_negation,`

<a id="libtmux-lowered-node-hpp-lowerednode-kind-begin-relation"></a>
#### `LoweredNode::Kind::begin_relation` — `begin_relation,`

<a id="libtmux-lowered-node-hpp-lowerednode-kind-end-relation"></a>
#### `LoweredNode::Kind::end_relation` — `end_relation,`

<a id="libtmux-lowered-node-hpp-nodecollector"></a>
### `NodeCollector`

Collects a lowered expression. This is the sink the relation builders use to capture their child, and it is a plain value so the result is copyable.

```cpp
class NodeCollector;
```

<a id="libtmux-lowered-node-hpp-nodecollector-string-test"></a>
#### `NodeCollector::string_test`

```cpp
void string_test(std::string_view field, std::string_view op, std::string_view operand);
```

<a id="libtmux-lowered-node-hpp-nodecollector-bool-test"></a>
#### `NodeCollector::bool_test`

```cpp
void bool_test(std::string_view field, bool expected);
```

<a id="libtmux-lowered-node-hpp-nodecollector-number-test"></a>
#### `NodeCollector::number_test`

```cpp
void number_test(std::string_view field, std::string_view op, long long operand);
```

<a id="libtmux-lowered-node-hpp-nodecollector-begin-group"></a>
#### `NodeCollector::begin_group`

```cpp
void begin_group(bool conjunction);
```

<a id="libtmux-lowered-node-hpp-nodecollector-end-group"></a>
#### `NodeCollector::end_group`

```cpp
void end_group();
```

<a id="libtmux-lowered-node-hpp-nodecollector-begin-negation"></a>
#### `NodeCollector::begin_negation`

```cpp
void begin_negation();
```

<a id="libtmux-lowered-node-hpp-nodecollector-end-negation"></a>
#### `NodeCollector::end_negation`

```cpp
void end_negation();
```

<a id="libtmux-lowered-node-hpp-nodecollector-begin-relation"></a>
#### `NodeCollector::begin_relation`

```cpp
void begin_relation(std::string_view name, int quantifier);
```

<a id="libtmux-lowered-node-hpp-nodecollector-end-relation"></a>
#### `NodeCollector::end_relation`

```cpp
void end_relation();
```

<a id="libtmux-lowered-node-hpp-nodecollector-nodes"></a>
#### `NodeCollector::nodes`

```cpp
[[nodiscard]] const LoweredExpression& nodes() const noexcept;
```

<a id="libtmux-lowered-node-hpp-nodecollector-take"></a>
#### `NodeCollector::take`

```cpp
[[nodiscard]] LoweredExpression take() noexcept;
```

<a id="libtmux-lowered-node-hpp-free-symbols"></a>
### `Free symbols`

<a id="libtmux-lowered-node-hpp-free-symbols-loweredexpression"></a>
#### `LoweredExpression`

```cpp
using LoweredExpression = std::vector<LoweredNode>;
```

<a id="libtmux-legacy-lookup-hpp"></a>
## `libtmux/legacy_lookup.hpp`

Edge parser for the Python `field__lookup=value` spelling.  This exists so a caller migrating from Python libtmux can hand a recorded filter string straight across. It is deliberately an edge: it produces the same FilterExpr the typed fields produce, so nothing downstream knows a string was ever involved, and the typed spelling stays the only way to write a new filter.

**Symbols:**

- [`LookupParseError`](#libtmux-legacy-lookup-hpp-lookupparseerror)
  - [`LookupParseError::missing_value`](#libtmux-legacy-lookup-hpp-lookupparseerror-missing-value)
  - [`LookupParseError::unknown_field`](#libtmux-legacy-lookup-hpp-lookupparseerror-unknown-field)
  - [`LookupParseError::unknown_lookup`](#libtmux-legacy-lookup-hpp-lookupparseerror-unknown-lookup)
- [`Free symbols`](#libtmux-legacy-lookup-hpp-free-symbols)
  - [`lookup_of`](#libtmux-legacy-lookup-hpp-free-symbols-lookup-of)
  - [`parse_lookup`](#libtmux-legacy-lookup-hpp-free-symbols-parse-lookup)

<a id="libtmux-legacy-lookup-hpp-lookupparseerror"></a>
### `LookupParseError`

```cpp
enum class LookupParseError;
```

<a id="libtmux-legacy-lookup-hpp-lookupparseerror-missing-value"></a>
#### `LookupParseError::missing_value` — `missing_value,`

<a id="libtmux-legacy-lookup-hpp-lookupparseerror-unknown-field"></a>
#### `LookupParseError::unknown_field` — `unknown_field,`

<a id="libtmux-legacy-lookup-hpp-lookupparseerror-unknown-lookup"></a>
#### `LookupParseError::unknown_lookup` — `unknown_lookup,`

<a id="libtmux-legacy-lookup-hpp-free-symbols"></a>
### `Free symbols`

<a id="libtmux-legacy-lookup-hpp-free-symbols-lookup-of"></a>
#### `lookup_of`

```cpp
[[nodiscard]] inline expected<StringOp, LookupParseError> lookup_of(std::string_view name);
```
`eq` is the implicit lookup, matching Python's bare `field=value`. An empty name reaches here only from a key that carried no separator at all; a key ending in one is refused before this is asked.

<a id="libtmux-legacy-lookup-hpp-free-symbols-parse-lookup"></a>
#### `parse_lookup`

```cpp
template <typename Entity> [[nodiscard]] expected<FilterExpr<Entity>, LookupParseError> parse_lookup(std::string_view term, std::span<const StringFieldHandle<Entity>> fields);
```
Parse one `field[__lookup]=value` term against a caller-supplied field table.  The value is taken verbatim after the first `=`, so a value containing `=` or `__` survives unchanged; only the key is split.

<a id="libtmux-expected-hpp"></a>
## `libtmux/expected.hpp`

The one C++23 library facility this package's public surface needs.  Recoverable failure is reported by value, not by exception, which means `std::expected`. That type arrived in C++23, so a toolchain shipping a C++20 standard library cannot provide it. Rather than fork the API, the C++20 build substitutes the reference implementation the standard type was modelled on.  Callers write `libtmux::expected` and never name either underlying type, so the same source compiles under both.

**Symbols:**

- [`Free symbols`](#libtmux-expected-hpp-free-symbols)
  - [`expected`](#libtmux-expected-hpp-free-symbols-expected)
  - [`expected`](#libtmux-expected-hpp-free-symbols-expected-2)
  - [`unexpected_t`](#libtmux-expected-hpp-free-symbols-unexpected-t)
  - [`unexpected_t`](#libtmux-expected-hpp-free-symbols-unexpected-t-2)
  - [`unexpected`](#libtmux-expected-hpp-free-symbols-unexpected)

<a id="libtmux-expected-hpp-free-symbols"></a>
### `Free symbols`

<a id="libtmux-expected-hpp-free-symbols-expected"></a>
#### `expected`

```cpp
template <typename Value, typename Error> using expected = tl::expected<Value, Error>;
```
Available when `defined(LIBTMUX_USE_TL_EXPECTED)`.

<a id="libtmux-expected-hpp-free-symbols-expected-2"></a>
#### `expected`

```cpp
template <typename Value, typename Error> using expected = std::expected<Value, Error>;
```
Available when `!(defined(LIBTMUX_USE_TL_EXPECTED))`.

<a id="libtmux-expected-hpp-free-symbols-unexpected-t"></a>
#### `unexpected_t`

```cpp
template <typename Error> using unexpected_t = tl::unexpected<Error>;
```
Available when `defined(LIBTMUX_USE_TL_EXPECTED)`.

<a id="libtmux-expected-hpp-free-symbols-unexpected-t-2"></a>
#### `unexpected_t`

```cpp
template <typename Error> using unexpected_t = std::unexpected<Error>;
```
Available when `!(defined(LIBTMUX_USE_TL_EXPECTED))`.

<a id="libtmux-expected-hpp-free-symbols-unexpected"></a>
#### `unexpected`

```cpp
template <typename Error> [[nodiscard]] constexpr auto unexpected(Error&& error);
```
A factory rather than an alias: an alias template cannot deduce its argument, so `unexpected(error)` would stop compiling at every call site.

<a id="libtmux-abi-hpp"></a>
## `libtmux/abi.hpp`

Binary identity.  The C++20 and C++23 builds are not ABI-compatible: every type that carries a result differs in layout because the underlying expected differs. Linking objects from both produces a program that appears to build and then reads the wrong bytes.  An inline namespace whose name encodes the choice makes that a link error naming the missing symbol instead. Callers still write `libtmux::Server`; the namespace is inline, so it is invisible in source and decisive in the mangled name.

**Symbols:**

- [`Free symbols`](#libtmux-abi-hpp-free-symbols)
  - [`LIBTMUX_ABI_NAMESPACE`](#libtmux-abi-hpp-free-symbols-libtmux-abi-namespace)
  - [`LIBTMUX_ABI_NAMESPACE`](#libtmux-abi-hpp-free-symbols-libtmux-abi-namespace-2)
  - [`LIBTMUX_NAMESPACE_BEGIN`](#libtmux-abi-hpp-free-symbols-libtmux-namespace-begin)

<a id="libtmux-abi-hpp-free-symbols"></a>
### `Free symbols`

<a id="libtmux-abi-hpp-free-symbols-libtmux-abi-namespace"></a>
#### `LIBTMUX_ABI_NAMESPACE`

```cpp
#define LIBTMUX_ABI_NAMESPACE v1_cxx20
```
Available when `defined(LIBTMUX_USE_TL_EXPECTED)`.

<a id="libtmux-abi-hpp-free-symbols-libtmux-abi-namespace-2"></a>
#### `LIBTMUX_ABI_NAMESPACE`

```cpp
#define LIBTMUX_ABI_NAMESPACE v1_cxx23
```
Available when `!(defined(LIBTMUX_USE_TL_EXPECTED))`.

<a id="libtmux-abi-hpp-free-symbols-libtmux-namespace-begin"></a>
#### `LIBTMUX_NAMESPACE_BEGIN`

```cpp
#define LIBTMUX_NAMESPACE_BEGIN                                                        \
namespace libtmux {                                                                  \
inline namespace LIBTMUX_ABI_NAMESPACE {
```
