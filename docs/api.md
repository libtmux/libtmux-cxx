# API reference

Generated from the headers by `tools/docs/api_index.py`; the prose here
is the prose there. Run it with `--check` to prove this page is current.

## `libtmux/libtmux.hpp`

libtmux: a typed C++ interface to tmux.  This umbrella header pulls in the dependency-free core: value types for the things tmux prints, snapshots that own what a command returned, entities projected from those snapshots, and expressions for selecting among them. Nothing here spawns a process; execution belongs to the connection type.

## `libtmux/server.hpp`

The connection root.  A Server names which tmux server to talk to and how to reach it. It is a handle: copying one costs a reference count, and every entity taken from it keeps the same connection alive, so a listing outlives the Server value that produced it.  No transport type appears in this header. `run` is declared here and defined against a private backend, so an async or control-mode executor can replace that backend without changing a caller or breaking ABI.

### Server

```cpp
[[nodiscard]] static expected<Server, CommandFailure> at_socket_path(std::string_view path, CommandObserver observer = {}, ExecutionPolicy policy = {});
```
`-S path`: the socket file, used verbatim.  These report `CommandFailure`, the same type every other call reports, rather than the `SocketError` the argument builders use: a factory that failed differently is a factory nothing can be chained onto. The reason a selector was rejected is in the diagnostic, and `socket_path_arguments` still returns the enum for a caller that wants to branch on it.  An observer, if given, is told about every command this server runs. It is fixed at construction because the connection is immutable afterwards, and that is what makes a Server safe to copy between threads. The policy is fixed for the same reason, and says what a call gets when it names no timeout or limit of its own.

```cpp
[[nodiscard]] static expected<Server, CommandFailure> at_socket_name(std::string_view name, CommandObserver observer = {}, ExecutionPolicy policy = {});
```
`-L name`: resolved under tmux's socket directory, as the tmux flag does.

```cpp
[[nodiscard]] static expected<Server, CommandFailure> from_env(CommandObserver observer = {}, ExecutionPolicy policy = {});
```
The server this process is running inside.  tmux exports `TMUX` to everything it starts, as `<socket path>,<server pid>,<session id>`. Only the socket path is read: the session id is stale the moment a pane moves, and a `#()` job carries no session at all — so a caller who wants the session asks tmux, rather than trusting what it inherited.

```cpp
[[nodiscard]] static expected<Server, CommandFailure> at_default(CommandObserver observer = {}, ExecutionPolicy policy = {});
```
The server tmux would talk to with no `-L` or `-S` at all, which is the one a person means when they say "my tmux".

```cpp
run(const std::vector<std::string>& command, std::optional<std::chrono::milliseconds> timeout = {}, std::optional<std::size_t> output_limit = {}) const;
```
The timeout still rides on the call: how long a caller will wait is a property of what they asked for, and listing sessions does not share a deadline with attaching a client. Unset takes the server's `ExecutionPolicy`, which is thirty seconds rather than forever — a floor, not a guess at what this particular command needs. `output_limit` bounds how much of tmux's answer this call will hold. Past it the command reports `truncated` rather than returning a prefix that reads like a complete answer. Unset uses the package default, which is ample for every listing and can be too small for a long scrollback.

```cpp
[[nodiscard]] expected<std::string, CommandFailure> run_batch(const CommandBatch& batch) const;
```
Run several commands in one invocation. tmux runs a batch fail-fast, so a failed batch is partially applied rather than rolled back, and one exit status covers the group: which member failed needs control mode.

```cpp
[[nodiscard]] expected<std::string, CommandFailure> run_chain(const Chain& chain) const;
```
Run a chain. A chain that failed validation never reaches tmux, and says which step was wrong rather than surfacing a tmux message about it.

```cpp
[[nodiscard]] expected<Connection, ProtocolError> control(std::string_view session) const;
```
Open a control-mode connection to one session.  This is the streaming half of the transport: a control connection stays open, gives each command its own reply block, and delivers asynchronous notifications between them. The synchronous surface above is unaffected — a caller who never opens one never pays for it.  Fails with `ProtocolError`, not `CommandFailure`, because that is what the `Connection` it returns speaks: an error type here that the value's own surface does not use would make the doorway disagree with the room. `over_control` returns an ordinary `Server` and so reports the ordinary failure.

```cpp
[[nodiscard]] std::vector<Notification> take_notifications() const;
```
What tmux has said on its own initiative since the last call: a window renamed, a pane exited, a client attached. A Server that runs a process per command hears nothing between them and answers with nothing, so a caller that wants this opens one with `over_control`.  The buffer is bounded; `dropped_notifications` says how many were discarded, which distinguishes a quiet server from one that outran a caller who was not collecting.

```cpp
[[nodiscard]] std::size_t dropped_notifications() const noexcept;
```

```cpp
[[nodiscard]] expected<Server, CommandFailure> over_control(std::string_view session) const;
```
The same surface, dispatched over one open control connection instead of a process per command. Entities taken from the result are ordinary entities; nothing above the transport knows the difference.  A connection carries one conversation, so commands over it are serialized. Two Servers over the same socket are two conversations.

```cpp
[[nodiscard]] expected<Version, CommandFailure> tmux_version() const;
```
Which tmux is behind this connection. `tmux -V` answers without touching the server, so this reports a version even when nothing is running.

```cpp
[[nodiscard]] bool is_alive(std::chrono::milliseconds timeout = std::chrono::seconds{ 5}) const;
```
Whether a server is answering on this socket. False covers every reason — no server, no socket, a tmux that would not run — because a caller who only wants to know whether to start one does not need to tell them apart.  Bounded by default: the one call whose whole job is answering "can I reach this" must not be the call that hangs. A stalled socket or a stopped server answers no, in time, rather than never.

```cpp
[[nodiscard]] expected<void, CommandFailure> check_alive(std::chrono::milliseconds timeout = std::chrono::seconds{5}) const;
```
The same question, keeping why the answer was no.

```cpp
[[nodiscard]] expected<void, CommandFailure> kill() const;
```
End the server and everything in it.

```cpp
[[nodiscard]] expected<std::vector<Session>, CommandFailure> sessions() const;
```
One snapshot each. Iterating or filtering the result never reaches tmux again; taking a current view means calling these again.

```cpp
[[nodiscard]] expected<std::vector<Window>, CommandFailure> windows() const;
```
tmux scopes window and pane listings to the current session unless asked for every one, so `-a` is part of the request rather than a caller's responsibility to remember.

```cpp
[[nodiscard]] expected<std::vector<Pane>, CommandFailure> panes() const;
```

```cpp
[[nodiscard]] expected<std::vector<Client>, CommandFailure> clients() const;
```

```cpp
[[nodiscard]] expected<void, CommandFailure> wait_for(std::string_view channel, std::optional<std::chrono::milliseconds> timeout = {}) const;
```
Block until someone signals this channel, or the deadline passes.  tmux latches a signal: one sent while nobody is waiting satisfies the next wait rather than being lost. That makes signal-before-wait safe, and it also means a stale signal can release a later waiter, so a channel is worth naming for one exchange rather than reusing.  A server that dies under a waiter makes tmux exit zero, which is indistinguishable from being signalled — a caller would carry on as though the other side had spoken. This reports that as a failure instead, which is the reason to prefer it over running the command.

```cpp
[[nodiscard]] expected<void, CommandFailure> signal(std::string_view channel) const;
```
Release whoever is waiting on the channel, or latch it for whoever waits next.

```cpp
[[nodiscard]] expected<std::vector<Command>, CommandFailure> commands() const;
```
Every command this tmux understands, with its alias and usage.  Asking beats inferring: the supported-version range is a floor, not a description, and a distribution can ship a build with commands left out. A caller deciding whether a capability exists can look.

```cpp
[[nodiscard]] expected<std::vector<Buffer>, CommandFailure> buffers() const;
```
The server's cut buffers, newest first as tmux orders them. A server holding none answers with an empty list rather than a failure, like every other listing here.

```cpp
[[nodiscard]] expected<void, CommandFailure> load_buffer(std::string_view name, const std::filesystem::path& from) const;
```
Read a file into a named buffer, and write one back out.  The file is read and written by the tmux server, so the path is the server's to resolve — which matters when it is not on this machine. The bytes are not interpreted: a buffer round-tripped through a file comes back identical.

```cpp
[[nodiscard]] expected<void, CommandFailure> save_buffer(std::string_view name, const std::filesystem::path& to) const;
```

```cpp
[[nodiscard]] expected<void, CommandFailure> bind_key(std::string_view table, std::string_view key, const std::vector<std::string>& command, bool repeatable = false) const;
```
Bind a key in a key table, and take a binding away again.  The command is argv, not a string, so nothing here has to be quoted for tmux to take it apart again correctly.  A table name containing whitespace is refused. tmux accepts one and then prints it unquoted in `list-keys`, where `-T my table X command` cannot be told apart from the table `my` bound to the key `table` — so a name that survives being listed is required, the same way a target refuses a name that cannot survive being parsed.  Key names are tmux's to check: unlike `send-keys`, `bind-key` reports an unknown one — including an empty one — so there is nothing for this to add, and nothing here repeats it.  `repeatable` is tmux's `-r`, letting the key repeat without the prefix being pressed again.

```cpp
[[nodiscard]] expected<void, CommandFailure> unbind_key(std::string_view table, std::string_view key) const;
```
Unbinding a key that was not bound succeeds; unbinding in a table that does not exist is refused. A table exists only while something is bound in it, so taking away the last binding takes the table with it.

```cpp
[[nodiscard]] expected<void, CommandFailure> run_shell(std::string_view command, bool background = false) const;
```
Run a shell command on the machine the server is on.  Reports whether it ran, not what it printed. tmux hands back the output on most versions and discards it on 3.3a and 3.4, and an answer that is empty on two versions in the middle of the range is worse than no answer at all. A caller that needs the output redirects it to a file.  What is uniform is the exit status: a command that fails is a failure here, carrying its code in `exit_code`.  `background` is tmux's `-b`, which returns as soon as the command is started. Nothing can then be said about how it ended, so a backgrounded command that fails still reports success.

```cpp
[[nodiscard]] expected<void, CommandFailure> source_file(const std::filesystem::path& file) const;
```
Run the commands in a file, the way tmux runs a configuration file.  The server reads the file, so the path is the server's to resolve. A file it cannot read is reported rather than passed over.

```cpp
[[nodiscard]] expected<void, CommandFailure> check_file(const std::filesystem::path& file) const;
```
Parse the same file and report what tmux would refuse in it, running none of it. This is how a program checks a configuration it is about to apply without applying half of it first.

```cpp
[[nodiscard]] expected<std::string, CommandFailure> expand(std::string_view format) const;
```
Ask tmux to expand a format, against no target but the server itself.  Unguarded, unlike the entity forms: there is no target here that could have gone away. A server kept alive with no sessions still answers its own fields, and answers the session-scoped ones with nothing — which is the truth about that server, not a failure to report.

```cpp
[[nodiscard]] expected<void, CommandFailure> show_message(std::string_view text) const;
```
Put a message on the status line of every attached client, and send it to each control client as `%message`.  tmux expands the text as a format, so a `#{...}` in it is substituted rather than shown. Text built from data belongs in `escape_literal` first.

```cpp
[[nodiscard]] expected<void, CommandFailure> set_buffer(std::string_view name, std::string_view data) const;
```
Put text in a named buffer, replacing what was there. An empty name lets tmux choose one, which is what a caller copying without caring about the name wants.

```cpp
[[nodiscard]] expected<Session, CommandFailure> session(std::string_view target) const;
```
One object by target, for a caller holding an id or a `session:window` path that came from somewhere else. The target is resolved the way tmux resolves it, so a session target names that session's active pane and the window that pane is in. A target tmux cannot resolve is reported missing.

```cpp
[[nodiscard]] expected<Window, CommandFailure> window(std::string_view target) const;
```

```cpp
[[nodiscard]] expected<Pane, CommandFailure> pane(std::string_view target) const;
```

```cpp
[[nodiscard]] expected<Session, CommandFailure> new_session(std::string_view name) const;
```
Created detached, and returned, because tmux prints what it made.

```cpp
[[nodiscard]] expected<Session, CommandFailure> new_session(NewSessionOptions options) const;
```

```cpp
[[nodiscard]] expected<std::vector<OptionEntry>, CommandFailure> options(std::string_view target = {}) const;
```

```cpp
[[nodiscard]] expected<std::vector<OptionEntry>, CommandFailure> server_options() const;
```
The server's own options, which are neither session nor window options and are the only ones a server without a session still has.

```cpp
[[nodiscard]] expected<void, CommandFailure> set_server_option(std::string_view name, std::string_view value) const;
```

```cpp
[[nodiscard]] expected<std::vector<OptionEntry>, CommandFailure> global_options() const;
```

```cpp
[[nodiscard]] expected<void, CommandFailure> set_global_option(std::string_view name, std::string_view value) const;
```
Sets the value every session inherits, rather than one session's own.

```cpp
[[nodiscard]] expected<std::vector<OptionEntry>, CommandFailure> hooks(std::string_view target = {}) const;
```

```cpp
[[nodiscard]] expected<std::vector<OptionEntry>, CommandFailure> global_hooks() const;
```
A hook set globally is not reported by the unscoped listing, so reading it back needs the scope it was set with.

```cpp
[[nodiscard]] expected<void, CommandFailure> set_global_hook(std::string_view name, std::string_view command) const;
```

## `libtmux/entities.hpp`

The tmux object hierarchy.  A Session, Window, Pane or Client is one row of a snapshot: a shared pointer to the listing it came from plus the index of its row. That representation is what lets an entity be copied, stored in a container and returned from a function with nothing to keep alive alongside it, while still costing no per-row allocation and no tmux call to read.  Reading a field is local and cannot fail. Every method returning `expected` runs tmux, and a returned entity describes the moment that command ran: entities do not update themselves, `refresh` takes a new snapshot.  Each entity addresses itself by tmux id (`$0`, `@0`, `%0`) rather than by name, so an operation cannot be redirected by a name that happens to contain a target separator.  Every field below is a format token tmux 3.2a already registers, which is the oldest version this library supports. That is a hard constraint rather than a preference: tmux expands a token it does not know to the empty string, so requesting a newer one would read as a present-but-empty value on an older server instead of failing.

### Session

```cpp
Session(std::shared_ptr<const Snapshot> snapshot, std::size_t row) noexcept : Row;
```

```cpp
[[nodiscard]] std::string_view id() const noexcept;
```

```cpp
[[nodiscard]] std::string_view name() const noexcept;
```

```cpp
[[nodiscard]] bool attached() const noexcept;
```
`session_attached` counts clients rather than rendering a flag, so any count other than zero means attached.

```cpp
[[nodiscard]] long long client_count() const noexcept;
```

```cpp
[[nodiscard]] long long window_count() const noexcept;
```

```cpp
[[nodiscard]] std::string_view path() const noexcept;
```
The directory a new window starts in, not the shell's current directory.

```cpp
[[nodiscard]] std::chrono::sys_seconds created() const noexcept;
```

```cpp
[[nodiscard]] std::string_view group() const noexcept;
```
Empty unless the session belongs to a group sharing its windows.

```cpp
[[nodiscard]] bool grouped() const noexcept;
```

```cpp
[[nodiscard]] expected<std::vector<Window>, CommandFailure> windows() const;
```

```cpp
[[nodiscard]] expected<std::vector<Pane>, CommandFailure> panes() const;
```

```cpp
[[nodiscard]] expected<Window, CommandFailure> active_window() const;
```

```cpp
[[nodiscard]] expected<Pane, CommandFailure> active_pane() const;
```

```cpp
[[nodiscard]] expected<Window, CommandFailure> select_next_window() const;
```
Move the selection, and answer with the window it landed on.  Named for what they do rather than for what they return: `next_window()` would read as a question, and these change which window is active.  Relative navigation is tmux's to perform, not a caller's to compute. Next and previous wrap around the window list, and "last" means the previously selected window — state only the server holds, which a caller listing windows has no way to reconstruct.  Each fails when there is nowhere to go, as tmux does: a session with one window refuses all three rather than selecting the window already active.

```cpp
[[nodiscard]] expected<Window, CommandFailure> select_previous_window() const;
```

```cpp
[[nodiscard]] expected<Window, CommandFailure> select_last_window() const;
```

```cpp
[[nodiscard]] expected<Window, CommandFailure> new_window(std::string_view name) const;
```
Created detached: a library call that stole the terminal would be a surprise, and attaching is a separate decision.

```cpp
[[nodiscard]] expected<Window, CommandFailure> new_window(NewWindowOptions options) const;
```

```cpp
[[nodiscard]] expected<void, CommandFailure> rename(std::string_view name) const;
```

```cpp
[[nodiscard]] expected<void, CommandFailure> kill() const;
```

```cpp
[[nodiscard]] expected<Session, CommandFailure> refresh() const;
```

```cpp
[[nodiscard]] expected<std::vector<OptionEntry>, CommandFailure> options() const;
```
Session options. Reading reports the value tmux would use, marking one that comes from a wider scope as inherited rather than hiding it.

```cpp
[[nodiscard]] expected<OptionEntry, CommandFailure> option(std::string_view name) const;
```

```cpp
[[nodiscard]] expected<void, CommandFailure> set_option(std::string_view name, std::string_view value) const;
```

```cpp
[[nodiscard]] expected<void, CommandFailure> unset_option(std::string_view name) const;
```
Remove the value set here, so the wider scope shows through again.

```cpp
[[nodiscard]] std::vector<std::string> attach_command() const;
```
The command line that attaches a terminal to this session.  Not a method that attaches: a tmux client needs a terminal, and every command this library runs talks to it through pipes, so an attach it performed itself could only ever fail. A caller that owns a terminal execs this instead.

```cpp
[[nodiscard]] expected<void, CommandFailure> detach_clients() const;
```
Send every client here away, leaving the session running.

```cpp
[[nodiscard]] expected<std::string, CommandFailure> expand(std::string_view format) const;
```
Ask tmux to expand a format against this session. The fields above are what a session is; this reaches the rest of tmux's vocabulary without a method per variable.  A target tmux cannot find is not an error to tmux. It expands the fields it cannot resolve to nothing, prints the literals around them, and exits zero — so a session that has been killed answers with a blank that reads like a value. This asks for the session's own id alongside the caller's format and reports `missing` when the answer is not this session.

```cpp
[[nodiscard]] expected<void, CommandFailure> show_message(std::string_view text) const;
```
Put a message on the status line of every client attached here, and send it to a control client as `%message`.  tmux expands the text as a format, so a `#{...}` in it is substituted rather than shown. Text built from data belongs in `escape_literal` first.

```cpp
[[nodiscard]] expected<std::vector<OptionEntry>, CommandFailure> hooks() const;
```

```cpp
[[nodiscard]] expected<void, CommandFailure> set_hook(std::string_view name, std::string_view command) const;
```

### Window

```cpp
Window(std::shared_ptr<const Snapshot> snapshot, std::size_t row) noexcept : Row;
```

```cpp
[[nodiscard]] std::string_view id() const noexcept;
```

```cpp
[[nodiscard]] std::string_view name() const noexcept;
```

```cpp
[[nodiscard]] bool active() const noexcept;
```

```cpp
[[nodiscard]] std::string_view session_id() const noexcept;
```
The link to the parent, carried in the row so traversal upward costs nothing until the parent itself is wanted.

```cpp
[[nodiscard]] long long index() const noexcept;
```
Position within its session, which `base-index` is free to start anywhere.

```cpp
[[nodiscard]] long long pane_count() const noexcept;
```

```cpp
[[nodiscard]] long long width() const noexcept;
```

```cpp
[[nodiscard]] long long height() const noexcept;
```

```cpp
[[nodiscard]] std::string_view layout() const noexcept;
```
tmux's own layout description, which `select-layout` accepts back.

```cpp
[[nodiscard]] bool zoomed() const noexcept;
```

```cpp
[[nodiscard]] bool bell() const noexcept;
```

```cpp
[[nodiscard]] bool activity() const noexcept;
```

```cpp
[[nodiscard]] long long linked_sessions() const noexcept;
```
How many sessions hold this window. More than one means the same window is shown in several places, and a command aimed at a bare id could land on any of them — which is why targets here are qualified.

```cpp
[[nodiscard]] std::string target() const;
```
How to address this window, and the reason a window id alone will not do.  The same window can be linked into several sessions, and a bare `@id` leaves tmux to pick one of those homes: the index it reports, the session it names, and the link a move or a kill lands on then depend on a choice the caller did not make. Qualifying by the session this window was listed from names one link.

```cpp
[[nodiscard]] expected<Session, CommandFailure> session() const;
```

```cpp
[[nodiscard]] expected<std::vector<Pane>, CommandFailure> panes() const;
```

```cpp
[[nodiscard]] expected<Pane, CommandFailure> active_pane() const;
```

```cpp
[[nodiscard]] expected<Pane, CommandFailure> split() const;
```

```cpp
[[nodiscard]] expected<Pane, CommandFailure> split(SplitOptions options) const;
```

```cpp
[[nodiscard]] expected<void, CommandFailure> rename(std::string_view name) const;
```

```cpp
[[nodiscard]] expected<void, CommandFailure> select_layout(std::string_view layout) const;
```
Rearrange the panes. tmux names five layouts and also accepts the layout description `layout()` returns, which is how a saved arrangement is restored exactly.

```cpp
[[nodiscard]] expected<void, CommandFailure> resize(long long width, long long height) const;
```

```cpp
[[nodiscard]] expected<void, CommandFailure> next_layout() const;
```
Exchange positions with another window, keeping both ids. Step through tmux's preset arrangements, and turn the panes within the one in use.  The layouts are tmux's list, not a caller's: asking for "the next one" is the only way to reach them without naming each. Rotating is a different act — it moves which pane occupies which cell and leaves the cells where they are.  None of the three refuses a window holding a single pane. tmux accepts all of them there and changes nothing, which is worth knowing before treating success as evidence that something moved.

```cpp
[[nodiscard]] expected<void, CommandFailure> previous_layout() const;
```

```cpp
[[nodiscard]] expected<void, CommandFailure> rotate() const;
```

```cpp
[[nodiscard]] expected<Pane, CommandFailure> select_last_pane() const;
```
Go back to the pane that was selected before the current one, and answer with it.  Server state, like the window equivalent: nothing in a listing says which pane that was. A window holding one pane is refused rather than reselecting it, which is what tmux does.

```cpp
[[nodiscard]] expected<void, CommandFailure> link_to(const Session& target) const;
```
Show this window in another session as well. The same window, not a copy: what runs in it is running in one place and shown in two.

```cpp
[[nodiscard]] expected<void, CommandFailure> unlink() const;
```
Stop showing it in the session this value came from.  tmux refuses to remove the last link rather than leaving a window no session holds, and that refusal is kept: a caller who meant to be rid of the window wants `kill`, which says so.

```cpp
[[nodiscard]] expected<void, CommandFailure> swap_with(const Window& other) const;
```

```cpp
[[nodiscard]] expected<void, CommandFailure> move_to(long long index) const;
```
Move to another index within the same session.

```cpp
[[nodiscard]] expected<std::string, CommandFailure> expand(std::string_view format) const;
```
Ask tmux to expand a format against this window: the same reach the session form gives, guarded the same way. A window that has gone reports `missing` rather than answering with a blank.

```cpp
[[nodiscard]] expected<void, CommandFailure> show_message(std::string_view text) const;
```
Show a message to the clients watching this window's session.  The window is the context the text expands in, not just who sees it: `#{window_name}` in a message sent from here names this window even while another is the active one.

```cpp
[[nodiscard]] expected<void, CommandFailure> select() const;
```

```cpp
[[nodiscard]] expected<void, CommandFailure> kill() const;
```

```cpp
[[nodiscard]] expected<Window, CommandFailure> refresh() const;
```

```cpp
[[nodiscard]] expected<std::vector<OptionEntry>, CommandFailure> options() const;
```
Window options. tmux looks a named option up in the table it belongs to, so the scope here selects which window provides the context and the inheritance chain, not which names are legal.

```cpp
[[nodiscard]] expected<OptionEntry, CommandFailure> option(std::string_view name) const;
```

```cpp
[[nodiscard]] expected<void, CommandFailure> set_option(std::string_view name, std::string_view value) const;
```

```cpp
[[nodiscard]] expected<void, CommandFailure> unset_option(std::string_view name) const;
```

### Pane

```cpp
Pane(std::shared_ptr<const Snapshot> snapshot, std::size_t row) noexcept : Row;
```

```cpp
[[nodiscard]] std::string_view id() const noexcept;
```

```cpp
[[nodiscard]] std::string_view command() const noexcept;
```
What is running in the pane now, which is not what started it.

```cpp
[[nodiscard]] bool active() const noexcept;
```

```cpp
[[nodiscard]] std::string_view window_id() const noexcept;
```

```cpp
[[nodiscard]] std::string_view session_id() const noexcept;
```

```cpp
[[nodiscard]] long long index() const noexcept;
```

```cpp
[[nodiscard]] std::string_view title() const noexcept;
```

```cpp
[[nodiscard]] long long pid() const noexcept;
```

```cpp
[[nodiscard]] std::string_view tty() const noexcept;
```

```cpp
[[nodiscard]] std::string_view path() const noexcept;
```

```cpp
[[nodiscard]] long long width() const noexcept;
```

```cpp
[[nodiscard]] long long height() const noexcept;
```

```cpp
[[nodiscard]] bool dead() const noexcept;
```
A pane whose program exited while `remain-on-exit` kept it on screen.

```cpp
[[nodiscard]] bool in_mode() const noexcept;
```
Copy mode and its relatives, in which sent keys move the cursor rather than reaching the program.

```cpp
[[nodiscard]] bool at_top() const noexcept;
```

```cpp
[[nodiscard]] bool at_bottom() const noexcept;
```

```cpp
[[nodiscard]] bool at_left() const noexcept;
```

```cpp
[[nodiscard]] bool at_right() const noexcept;
```

```cpp
[[nodiscard]] bool piping() const noexcept;
```
Whether this pane's output is currently being copied to a command.

```cpp
[[nodiscard]] expected<Window, CommandFailure> window() const;
```

```cpp
[[nodiscard]] expected<Session, CommandFailure> session() const;
```

```cpp
[[nodiscard]] expected<void, CommandFailure> send_text(std::string_view text) const;
```
Literal text, never interpreted as key names or formats, and never followed by a newline the caller did not ask for.

```cpp
[[nodiscard]] expected<void, CommandFailure> send_key(std::string_view key) const;
```

```cpp
[[nodiscard]] expected<std::string, CommandFailure> capture() const;
```
The visible contents, as tmux printed them. `capture_lines` frames it into lines, and takes a named string: the lines are views into it, so framing this return value directly is a compile error rather than a dangling read.  A pane's scrollback can be far larger than the default bound, and a capture that does not fit is reported rather than cut, so a caller reading history passes the size it is prepared to hold.

```cpp
[[nodiscard]] expected<std::string, CommandFailure> capture(CaptureOptions options) const;
```

```cpp
[[nodiscard]] expected<void, CommandFailure> set_width(long long width) const;
```

```cpp
[[nodiscard]] expected<void, CommandFailure> set_height(long long height) const;
```

```cpp
[[nodiscard]] expected<void, CommandFailure> swap_with(const Pane& other) const;
```

```cpp
[[nodiscard]] expected<Window, CommandFailure> break_out(std::string_view name = {}) const;
```
Take this pane out of its window and into a new one, which is returned. An empty name leaves tmux to name the window after what is running.

```cpp
[[nodiscard]] expected<void, CommandFailure> join(const Window& target) const;
```
Move this pane into another window, splitting it. The other half of `break_out`: that takes the tree apart, this puts it back.  The pane keeps its id, so a value held across the move still names it. The window it came from disappears if it held nothing else, which is why the target is named rather than inferred from where this pane is.

```cpp
[[nodiscard]] expected<void, CommandFailure> enter_copy_mode() const;
```
Forget the scrollback, which is the only way to bound a pane's memory without restarting what is running in it. Put this pane into copy mode, where its contents can be scrolled and selected rather than typed into.  Needs no attached client: the mode is pane state, which `in_mode` reports. Entering twice is harmless.

```cpp
[[nodiscard]] expected<void, CommandFailure> leave_mode() const;
```
Leave whatever mode the pane is in.  A pane in no mode is refused, with tmux's "not in a mode". That is kept rather than smoothed into success: checking first would cost a round trip and still race, and a caller who cares can read `in_mode` or ignore the failure.

```cpp
[[nodiscard]] expected<void, CommandFailure> pipe_to(std::string_view command) const;
```
Copy everything this pane prints to a shell command, until told to stop. The command runs on the tmux server's machine with the pane's output on its standard input.  Starting a second pipe replaces the first: tmux keeps one per pane, so there is nothing to close and nothing to leak.

```cpp
[[nodiscard]] expected<void, CommandFailure> stop_piping() const;
```
Stop copying. Harmless on a pane that was not piping.

```cpp
[[nodiscard]] expected<void, CommandFailure> set_title(std::string_view title) const;
```
Name this pane. The title is what `#{pane_title}` reports and what a status line can show; it survives the process being replaced.

```cpp
[[nodiscard]] expected<void, CommandFailure> respawn(bool replace_running = false) const;
```
Start the pane's command again.  tmux refuses a pane whose process is still running unless told to kill it, and that refusal is kept rather than smoothed over: replacing a live process is a decision, so `replace_running` has to be asked for.

```cpp
[[nodiscard]] expected<void, CommandFailure> clear_history() const;
```

```cpp
[[nodiscard]] expected<std::string, CommandFailure> expand(std::string_view format) const;
```
Ask tmux to expand a format against this pane. `#{pane_current_command}` and `#{pane_current_path}` are the two most callers reach for, and neither is a field this class carries: both change under a value that stays still.  Guarded like the session and window forms, because tmux answers a pane that has gone with a blank and a zero exit status.

```cpp
[[nodiscard]] expected<void, CommandFailure> show_message(std::string_view text) const;
```
Show a message to the clients watching this pane's session, expanded against this pane.

```cpp
[[nodiscard]] expected<void, CommandFailure> paste(const Buffer& buffer, bool consume = false) const;
```
Deliver a buffer's text to this pane, as if it had been typed. The text arrives on the command line and is not run: a caller wanting it executed sends Enter afterwards, which is the same distinction `send_text` draws.  `consume` is tmux's `-d`, deleting the buffer once it has been pasted, which is what a caller treating it as a one-shot transfer wants.

```cpp
[[nodiscard]] expected<void, CommandFailure> select() const;
```

```cpp
[[nodiscard]] expected<void, CommandFailure> kill() const;
```

```cpp
[[nodiscard]] expected<Pane, CommandFailure> refresh() const;
```

```cpp
[[nodiscard]] expected<std::vector<OptionEntry>, CommandFailure> options() const;
```
Pane options, the narrowest scope tmux has, and the end of an inheritance chain that runs pane, window, session, global.

```cpp
[[nodiscard]] expected<OptionEntry, CommandFailure> option(std::string_view name) const;
```

```cpp
[[nodiscard]] expected<void, CommandFailure> set_option(std::string_view name, std::string_view value) const;
```

```cpp
[[nodiscard]] expected<void, CommandFailure> unset_option(std::string_view name) const;
```

### Command

```cpp
Command(std::shared_ptr<const Snapshot> snapshot, std::size_t row) noexcept : Row;
```

```cpp
[[nodiscard]] std::string_view name() const noexcept;
```

```cpp
[[nodiscard]] std::string_view alias() const noexcept;
```
tmux's short form, such as `lscm` for `list-commands`. Empty when the command has none.

```cpp
[[nodiscard]] std::string_view usage() const noexcept;
```
The flags and arguments, as tmux prints them in its own help.

### Buffer

```cpp
Buffer(std::shared_ptr<const Snapshot> snapshot, std::size_t row) noexcept : Row;
```

```cpp
[[nodiscard]] std::string_view name() const noexcept;
```
Named by the caller, or by tmux as `buffer0` and upward when it is not.

```cpp
[[nodiscard]] long long size() const noexcept;
```

```cpp
[[nodiscard]] std::string_view sample() const noexcept;
```
The opening of the contents, as tmux prints it in a listing. Truncated, and with control characters rendered — `contents()` reads the bytes.

```cpp
[[nodiscard]] std::chrono::sys_seconds created() const noexcept;
```

```cpp
[[nodiscard]] expected<std::string, CommandFailure> contents() const;
```
The whole contents. tmux prints them with no trailing newline, so what comes back is exactly what was put in.

```cpp
[[nodiscard]] expected<void, CommandFailure> remove() const;
```

### Client

```cpp
Client(std::shared_ptr<const Snapshot> snapshot, std::size_t row) noexcept : Row;
```

```cpp
[[nodiscard]] std::string_view name() const noexcept;
```
A client is named by its terminal path, which is the only stable handle tmux gives; there is no client id format.

```cpp
[[nodiscard]] std::string_view session_name() const noexcept;
```

```cpp
[[nodiscard]] bool read_only() const noexcept;
```

```cpp
[[nodiscard]] std::string_view tty() const noexcept;
```

```cpp
[[nodiscard]] long long width() const noexcept;
```

```cpp
[[nodiscard]] long long height() const noexcept;
```

```cpp
[[nodiscard]] std::chrono::sys_seconds created() const noexcept;
```

```cpp
[[nodiscard]] std::chrono::sys_seconds last_activity() const noexcept;
```

```cpp
[[nodiscard]] std::string_view terminal() const noexcept;
```

```cpp
[[nodiscard]] bool control_mode() const noexcept;
```
A control-mode client is a program driving tmux, not a terminal.

```cpp
[[nodiscard]] expected<Session, CommandFailure> session() const;
```

```cpp
[[nodiscard]] expected<void, CommandFailure> switch_to(const Session& session) const;
```
Point this client at another session, leaving it attached.

```cpp
[[nodiscard]] expected<void, CommandFailure> detach() const;
```

```cpp
[[nodiscard]] expected<void, CommandFailure> refresh() const;
```
Redraw, and tell tmux the size this client is now, which matters for a control-mode client whose size tmux cannot otherwise observe.

### Free functions

```cpp
std::ostream& operator<<(std::ostream& stream, const Session& session);
```
Written as tmux would name it, with the detail that identifies it: an id and the thing a reader recognises it by. Declared against a forward-declared stream so no consumer pays for <ostream> to include an entity.

```cpp
std::ostream& operator<<(std::ostream& stream, const Window& window);
```

```cpp
std::ostream& operator<<(std::ostream& stream, const Pane& pane);
```

```cpp
std::ostream& operator<<(std::ostream& stream, const Client& client);
```

```cpp
[[nodiscard]] std::size_t operator()(const libtmux::Session& value) const noexcept;
```

```cpp
[[nodiscard]] std::size_t operator()(const libtmux::Window& value) const noexcept;
```

```cpp
[[nodiscard]] std::size_t operator()(const libtmux::Pane& value) const noexcept;
```

```cpp
[[nodiscard]] std::size_t operator()(const libtmux::Client& value) const noexcept;
```

## `libtmux/snapshot.hpp`

tmux format requests and the snapshots their output becomes.  A snapshot is everything one tmux listing returned: the bytes, the rows parsed out of them, and the connection that produced them. Entities are a shared pointer to one of these plus a row index, which is why an entity can be copied, stored and returned with no owner to keep alive, and why iterating a filtered range never spawns tmux — the process ran once, when the snapshot was taken.  A snapshot is a moment. Nothing in it changes when tmux does; asking for current state means taking another one.

### Free functions

```cpp
[[nodiscard]] inline std::string format_request(std::span<const std::string_view> fields);
```
Build the format argument for one entity's fields, terminating every field so a trailing empty value is still a value rather than a missing column.  The two substitutions nest rather than run in sequence, because tmux applies the inner one to the raw value and the outer one to its result. In that order a value already holding the escape marker is neutralised before the separator pass can produce one; reversed, the two become indistinguishable.  `#{s/…/…/:…}` predates every tmux this library supports, and neither character is a regular-expression metacharacter.  Unconditional, rather than applied only to the fields that could carry a separator. Expanding the substitutions costs about 0.32us per row — a 61-row listing pays 19us, against a process launch of some milliseconds — and a per-field exemption list is a thing to get wrong later, once, silently.

```cpp
[[nodiscard]] inline std::size_t decode_value(char* begin, std::size_t size) noexcept;
```
Undo that escaping, in place.  Escaping only ever lengthens, so the decoded bytes fit where the encoded ones were: the write cursor never overtakes the read cursor, nothing moves, and nothing is allocated. Answers the decoded length.  An escape marker followed by anything else is left as written. Output this library asked for contains no such sequence, and failing on one would mean a recording could not carry a literal `␛`.

```cpp
[[nodiscard]] inline bool split_row(std::string_view line, std::size_t fields, std::vector<std::string_view>& values);
```
Split one tmux output line into its field values.  The trailing separator emitted by `format_request` produces one empty tail element, which is dropped; a short or long row is reported rather than padded so a format-name typo cannot masquerade as an empty field.

### Snapshot

```cpp
[[nodiscard]] static std::shared_ptr<const Snapshot> from_recording(std::span<const std::string_view> fields, std::string output);
```
Output that did not come from a live server: a recording, a fixture, a test. Entities read and filter exactly as they would from a listing, and anything that would run a command reports that there is no connection. Returns null if a row does not match the fields it claims to have.

```cpp
Snapshot(const Snapshot&) = delete;
```

```cpp
Snapshot(Snapshot&&) = delete;
```

```cpp
~Snapshot();
```

```cpp
[[nodiscard]] std::size_t index_of(std::string_view field) const noexcept;
```

```cpp
[[nodiscard]] const std::vector<std::vector<std::string_view>>& rows() const noexcept;
```

## `libtmux/filter_expr.hpp`

Value-semantic filter expressions over explicit snapshots.  An expression owns every operand it compares against, so it can outlive the call that built it and can be stored, copied, and later translated. It is deliberately not an expression template: the node set is a closed variant, so the same value that filters a range in memory can be inspected and, later, compiled to a tmux `-f` format string.

### Combine

```cpp
std::string_view (*read)(const Entity&);
```

### Free functions

```cpp
bool (*read)(const Entity&);
```

```cpp
long long (*read)(const Entity&);
```

```cpp
explicit FilterExpr(Node node) : node_;
```

```cpp
FilterExpr(const FilterExpr& other) : node_;
```

```cpp
FilterExpr(FilterExpr&&) noexcept = default;
```

```cpp
~FilterExpr() = default;
```

```cpp
[[nodiscard]] bool operator()(const Entity& entity) const;
```

```cpp
[[nodiscard]] const Node& node() const noexcept;
```

```cpp
static Node clone(const Node& node);
```

```cpp
static bool evaluate(const StringTest& test, const Entity& entity);
```

```cpp
static bool evaluate(const BoolTest& test, const Entity& entity);
```

```cpp
static bool evaluate(const NumberTest& test, const Entity& entity);
```

```cpp
static bool evaluate(const Group& group, const Entity& entity);
```

```cpp
static bool evaluate(const Negation& negation, const Entity& entity);
```

```cpp
static bool evaluate(const RelationTest& test, const Entity& entity);
```

```cpp
static constexpr char fold(char value) noexcept;
```

```cpp
[[nodiscard]] FilterExpr<Entity> iequals(std::string_view operand) const;
```

```cpp
[[nodiscard]] FilterExpr<Entity> contains(std::string_view operand) const;
```

```cpp
[[nodiscard]] FilterExpr<Entity> starts_with(std::string_view operand) const;
```

```cpp
[[nodiscard]] FilterExpr<Entity> ends_with(std::string_view operand) const;
```

```cpp
[[nodiscard]] FilterExpr<Entity> make(StringOp op, std::string_view operand) const;
```

```cpp
[[nodiscard]] FilterExpr<Entity> operator<(long long operand) const;
```

```cpp
[[nodiscard]] FilterExpr<Entity> operator>(long long operand) const;
```

```cpp
[[nodiscard]] FilterExpr<Entity> make(NumberOp op, long long operand) const;
```

```cpp
[[nodiscard]] operator FilterExpr<Entity>() const;
```

```cpp
[[nodiscard]] FilterExpr<Entity> operator&&(FilterExpr<Entity> left, FilterExpr<Entity> right);
```

```cpp
std::make_unique<FilterExpr<Entity>>(std::move(operand))}};
```

```cpp
[[nodiscard]] FilterExpr<Entity> operator&&(FilterExpr<Entity> left, BoolFieldHandle<Entity> right);
```

```cpp
[[nodiscard]] FilterExpr<Entity> operator&&(BoolFieldHandle<Entity> left, FilterExpr<Entity> right);
```

### RelationTest

```cpp
std::function<bool(const Entity&)> evaluate;
```

## `libtmux/relations.hpp`

Relation quantifiers over to-many and to-one links.  The quantifier is named rather than inferred so a reader never has to guess what an empty relation means: `all_of` is satisfied by an empty relation, `any_of` is not, and `none_of` is. That is the vacuous-truth convention the standard algorithms already use, stated explicitly because getting it wrong silently changes which entities a filter returns.

### Quantifier

```cpp
[[nodiscard]] auto children_of(const std::vector<Child>& rows, StringFieldHandle<Child> foreign_key, StringFieldHandle<Parent> key);
```

### Free functions

```cpp
template <typename Parent, typename Child> auto children_of(const std::vector<Child>&&, StringFieldHandle<Child>, StringFieldHandle<Parent>) = delete;
```
The rows are borrowed, so a listing that dies at the semicolon is refused here rather than dangling inside the predicate later.

```cpp
[[nodiscard]] auto parent_of(const std::vector<Parent>& rows, StringFieldHandle<Child> foreign_key, StringFieldHandle<Parent> key);
```

```cpp
[[nodiscard]] FilterExpr<Entity> quantified(std::string name, Quantifier quantifier, auto read, auto predicate);
```

```cpp
const FilterExpr<RelatedMany<Entity, decltype(read)>> test;
```

```cpp
decltype(auto) related = read(entity);
```
`decltype(auto)`, not `const auto&`: an accessor that returns a view by value must stay non-const to be iterated at all, since a filtered view caches its first position. An accessor returning a container reference still binds without copying.

```cpp
lower(test, collector);
```

```cpp
std::move(name), static_cast<int>(quantifier), std::move(evaluate), collector.take()}};
```

```cpp
[[nodiscard]] FilterExpr<Entity> any_of(std::string name, auto read, auto predicate);
```

```cpp
[[nodiscard]] FilterExpr<Entity> all_of(std::string name, auto read, auto predicate);
```

```cpp
[[nodiscard]] FilterExpr<Entity> none_of(std::string name, auto read, auto predicate);
```

```cpp
[[nodiscard]] FilterExpr<Entity> is(std::string name, auto read, auto predicate);
```

```cpp
const FilterExpr<RelatedOne<Entity, decltype(read)>> test;
```

```cpp
lower(test, collector);
```

```cpp
std::move(name), static_cast<int>(Quantifier::is), std::move(evaluate), collector.take()}};
```

```cpp
template <typename Child, typename Parent> auto parent_of(const std::vector<Parent>&&, StringFieldHandle<Child>, StringFieldHandle<Parent>) = delete;
```

## `libtmux/cardinality.hpp`

Exception-free cardinality over snapshot views.  Callers ask for one entity far more often than they want to handle a range, and the two ways that request can fail are not the same failure: finding nothing is ordinary, finding several means the caller's filter was wrong. These return types keep both outcomes in the value channel.

### CardinalityError

```cpp
[[nodiscard]] constexpr std::string_view to_string(CardinalityError error) noexcept;
```

### Free functions

```cpp
[[nodiscard]] std::optional<Referenced<Range>> first(Range& range);
```

```cpp
[[nodiscard]] expected<Referenced<Range>, CardinalityError> exactly_one(Range& range);
```

## `libtmux/command.hpp`

Why a tmux command produced no answer.  `refused` means tmux ran and said no; `missing` means tmux ran, said yes, and the object asked about was not there; `truncated` means it answered at greater length than the caller allowed for. Every other value means tmux never got that far. They stay apart because the caller's next move differs — a rejected argument is a bug, a spawn failure is an environment problem, a timeout may be worth retrying, and a missing object is ordinary in a program that races a user closing a pane.

### Free functions

```cpp
[[nodiscard]] constexpr std::string_view to_string(FailureKind kind) noexcept;
```

```cpp
std::function<void(std::string_view command, const CommandFailure* failure)>;
```

## `libtmux/options.hpp`

Parse `show-options`-shaped output.  tmux prints one option per line as `name value`, and an array option as `name[index] value`. The value may be quoted when it contains spaces, and an option with an empty value prints its name alone. Keeping all three shapes in one parser means callers never hand-split option output again.

### Free functions

```cpp
[[nodiscard]] inline std::string unquote(std::string_view value);
```
Undo the quoting tmux applies when it prints a value.  tmux picks one of four forms, and a reader that knows only one corrupts the rest: `''` for an empty value; double quotes when the value contains any of ` #';${}`; single quotes when it contains a double quote; and otherwise no quotes at all. Inside any of them the body is escaped the way `vis` does it — `\t`, `\n`, `\\`, `\ooo` for a byte with no printable form — and a leading tilde is escaped whether or not anything else is.  This is the inverse of tmux's own `args_escape`, so a value read here and written back is the value that was there.

```cpp
[[nodiscard]] inline std::optional<OptionEntry> parse_option(std::string_view line);
```
Parse one line. Returns nullopt for a blank line so callers can feed raw output straight in.

```cpp
[[nodiscard]] inline std::vector<OptionEntry> parse_options(std::string_view output);
```

## `libtmux/control.hpp`

Decode tmux's control protocol.  A control-mode stream interleaves command reply blocks with asynchronous notifications. This parser turns bytes into those events and nothing else: no threads, no process, no executor. Feeding it is the caller's job, which is what lets the same decoder serve a synchronous read loop today and an async executor later without either appearing in this header.  Framing preserves bytes. A line inside a block that looks like a notification stays block body, because control-mode framing does not make it independently attributable, and block bodies are never converted to UTF-8.

### Free functions

```cpp
[[nodiscard]] std::string_view to_string(NotificationKind kind) noexcept;
```

```cpp
[[nodiscard]] ParsedNotification parse(const Notification& notification);
```

```cpp
ParsedNotification parse(Notification&&) = delete;
```

### Parser

```cpp
Parser() = default;
```

```cpp
Parser(std::size_t retained_reply_bytes, std::size_t line_bytes) noexcept : retained_reply_bytes_;
```
Zero means unbounded, which only a test that owns both ends should ask for.

```cpp
expected<std::vector<Event>, ProtocolError> feed(std::span<const std::byte> bytes);
```

```cpp
expected<void, ProtocolError> finish();
```

### Connection

```cpp
static expected<Connection, ProtocolError> connect(ConnectionOptions options);
```

```cpp
~Connection() noexcept;
```

```cpp
Connection(Connection&&) noexcept;
```

```cpp
Connection(const Connection&) = delete;
```

```cpp
ControlRequestResult execute(ControlRequest request, std::chrono::steady_clock::time_point deadline);
```

```cpp
[[nodiscard]] std::vector<Notification> take_notifications();
```
Everything tmux has said since the last call, and how many were dropped to keep the buffer bounded.

```cpp
[[nodiscard]] std::vector<Notification> wait_for_notifications(std::chrono::steady_clock::time_point deadline);
```
The same, but waits for something to arrive.  `take_notifications` returns immediately, so a caller reacting to tmux had to call it in a loop and sleep between — which either wakes too often or reacts too late, and picks that trade with no idea how long the next event will take. This blocks until at least one notification is available, the connection fails, or the deadline passes, and returns whatever it has.  An empty result means the deadline passed or the stream ended; the two are told apart by asking `execute` or `shutdown`, which report the failure. Notifications already buffered are returned without waiting at all.

```cpp
[[nodiscard]] std::size_t dropped_notifications() const noexcept;
```

```cpp
[[nodiscard]] std::int64_t native_child_pid() const noexcept;
```

```cpp
expected<void, ProtocolError> shutdown(std::chrono::steady_clock::time_point deadline);
```

## `libtmux/batch.hpp`

Build one tmux command sequence from several commands.  tmux accepts multiple commands in a single invocation separated by a `;` argument. Because the core execs argv directly and never goes through a shell, the separator is a bare `;` element — there is no backslash to escape and no quoting to get wrong.  A batch is one fail-fast group: tmux stops at the first command that errors. That is why a batch is a distinct type from a list of independent requests, which the transport runs separately and attributes individually.

### CommandBatch

```cpp
bool add(std::vector<std::string> command);
```
Append one command. An empty command is rejected rather than emitted, because an empty argv between separators makes tmux read the next command's name as an argument.

```cpp
[[nodiscard]] std::size_t size() const noexcept;
```

```cpp
[[nodiscard]] bool empty() const noexcept;
```

```cpp
[[nodiscard]] std::vector<std::string> argv() const;
```
Render the whole batch as one argv. A single command renders with no separator, so a batch of one is byte-identical to running it alone.

```cpp
[[nodiscard]] const std::vector<std::vector<std::string>>& commands() const noexcept;
```

## `libtmux/chain.hpp`

Compose several tmux commands as one fail-fast group.  A chain is a typed front for a batch: each step validates its own arguments as it is added, so a bad target or key name is reported where it was written rather than as a tmux message about a command the caller cannot see.  The chain records the first validation failure and stops accumulating. That keeps the fluent form honest — the alternative, throwing mid-expression or silently dropping a step, both leave the caller guessing which parts ran.

### Chain

```cpp
Chain& new_session(std::string_view name, bool detached = true);
```

```cpp
Chain& new_window(std::string_view session, std::string_view name);
```

```cpp
Chain& split_window(std::string_view session, std::string_view window);
```

```cpp
Chain& send_text(std::string_view target, std::string_view text);
```
Literal text, never interpreted as key names or formats.

```cpp
Chain& send_key(std::string_view target, std::string_view key);
```

```cpp
Chain& command(std::vector<std::string> argv);
```
Escape hatch for a command the typed steps do not cover.

```cpp
[[nodiscard]] bool valid() const noexcept;
```

```cpp
[[nodiscard]] const std::string& error() const noexcept;
```

```cpp
[[nodiscard]] const CommandBatch& batch() const noexcept;
```

## `libtmux/keys.hpp`

Build `send-keys` arguments.  tmux does not report an unknown key name: `send-keys NoSuchKey` succeeds silently, so a typo is invisible at the call site and shows up later as input that never arrived. Key names are therefore validated here, where the caller can still be told.  Literal text takes `-l`, under which tmux interprets nothing — not key names, not formats. Choosing between the two is the caller's decision and is never inferred from the text.

### KeyError

```cpp
[[nodiscard]] constexpr std::string_view to_string(KeyError error) noexcept;
```

### Free functions

```cpp
[[nodiscard]] inline bool is_function_key(std::string_view name) noexcept;
```

```cpp
[[nodiscard]] inline bool is_key_name(std::string_view key) noexcept;
```
Accept a key with any number of C-, M-, or S- modifiers.

```cpp
[[nodiscard]] inline expected<std::vector<std::string>, KeyError> literal_arguments(std::string_view text);
```
Send text exactly as written: the flag, the end of flags, and the text.  `--` is part of the fragment rather than something a caller appends, because text beginning with a dash is read as another `send-keys` option without it and that is not a mistake worth making twice. It was made twice: this returned the flag and the text alone, `Pane::send_text` inserted the separator afterwards, and `Chain::send_text` did not — so the same text through the two spellings reached tmux as two different commands.

## `libtmux/capture.hpp`

Split `capture-pane -p` output into lines.  Every captured line is newline-terminated, so a naive split on '\n' yields a final empty element that is not a line. Blank rows inside the pane are genuine empty lines and must survive, which is why the terminator is removed by dropping exactly one trailing element rather than by trimming empties.  `-p` already strips trailing whitespace from each line; `-N` preserves it. Neither is re-implemented here: the caller chooses the flag and this only frames what tmux returned.

### Free functions

```cpp
template <typename Text> requires std::same_as<std::remove_cvref_t<Text>, std::string> && (!std::is_lvalue_reference_v<Text>) std::vector<std::string_view> capture_lines(Text&&) = delete;
```
The lines are views into the text, so text that dies at the semicolon takes them with it — and `pane.capture()` returns its output by value, which makes `capture_lines(*pane.capture())` the natural thing to write and a use-after-free to run. Deleted for an rvalue string only: an lvalue string converts as before, and so does a literal.

```cpp
[[nodiscard]] inline std::vector<std::string_view> capture_lines(std::string_view output);
```

```cpp
[[nodiscard]] inline std::vector<std::string_view> without_trailing_blanks(std::vector<std::string_view> lines);
```
Drop the blank rows a pane pads its height with, keeping blank lines that have content below them.

## `libtmux/target.hpp`

Build tmux target specifiers.  tmux addresses objects either by id (`$0`, `@0`, `%0`) or by the `session:window.pane` path. Ids are unambiguous; the path is not, because `:` and `.` are its separators and a session or window name containing one silently re-parses as a different target. Prefer an id whenever one exists, and refuse to build a path from a name that cannot survive it.

### Free functions

```cpp
[[nodiscard]] constexpr std::string_view to_string(TargetError error) noexcept;
```

```cpp
[[nodiscard]] constexpr bool is_pane_id(std::string_view value) noexcept;
```
Whether a value is written as an id, which is what decides that it needs no separator validation below — not whether tmux would resolve it. `%x` is an id by this test and no pane by tmux's, and both are right: it carries no separator, so composing it can only produce the caller's own mistake back.

```cpp
[[nodiscard]] constexpr bool is_window_id(std::string_view value) noexcept;
```

```cpp
[[nodiscard]] constexpr bool is_session_id(std::string_view value) noexcept;
```

```cpp
[[nodiscard]] inline expected<std::string, TargetError> path_component(std::string_view name);
```
Validate one path component. Ids skip validation because they contain no separator by construction.

```cpp
[[nodiscard]] inline expected<std::string, TargetError> session_target(std::string_view session);
```
A session target is its id, or its validated name.

```cpp
[[nodiscard]] inline expected<std::string, TargetError> window_target(std::string_view session, std::string_view window);
```
A window target is its id, which needs no session, or `session:window`.

```cpp
[[nodiscard]] inline expected<std::string, TargetError> pane_target(std::string_view session, std::string_view window, std::string_view pane);
```
A pane target is its id, or `session:window.pane`.

## `libtmux/socket.hpp`

Build the connection arguments that select a tmux server.  tmux selects a server either by socket name (`-L`) or by socket path (`-S`). A name is resolved under the socket directory, so it must be a single path component; a path is used verbatim. Either way the result must fit a UNIX domain socket address, and exceeding that limit fails at connect time with a message that names neither the limit nor the path — so the check belongs here, where the argv is built.

### Free functions

```cpp
[[nodiscard]] constexpr std::string_view to_string(SocketError error) noexcept;
```

```cpp
[[nodiscard]] inline expected<std::vector<std::string>, SocketError> socket_name_arguments(std::string_view name);
```
`-L name`: a single component resolved under the socket directory.

```cpp
[[nodiscard]] inline expected<std::vector<std::string>, SocketError> socket_path_arguments(std::string_view path);
```
`-S path`: used verbatim, so the address limit applies to it directly.

## `libtmux/format.hpp`

Compose tmux format strings.  tmux treats `#` as the start of a substitution: `#{...}` expands a variable, `#(...)` runs a command, and `##` is a literal `#`. Any literal text placed in a format must therefore escape its `#` characters, or a status line containing `#1` silently becomes an expansion attempt.

### Free functions

```cpp
[[nodiscard]] inline std::string escape_literal(std::string_view text);
```
Escape literal text for inclusion in a format string.

```cpp
[[nodiscard]] inline std::string variable(std::string_view name);
```
Wrap a variable name as a substitution. The name is not escaped: a name is chosen by the caller from tmux's format vocabulary, not built from data.

### FormatBuilder

```cpp
FormatBuilder& literal(std::string_view text);
```

```cpp
FormatBuilder& field(std::string_view name);
```

```cpp
[[nodiscard]] const std::string& str() const noexcept;
```

## `libtmux/version.hpp`

Parse and order tmux version strings.  `tmux -V` prints `tmux 3.4`, `tmux 3.7a`, or a development build as `tmux next-3.8` or `tmux master`. The letter suffix orders after the bare release (3.7 < 3.7a < 3.7b), a `next-` build sorts before the release it leads to, and `master` is unbounded — treating any of these as a plain number silently mis-gates a capability check.

### VersionError

```cpp
[[nodiscard]] constexpr std::string_view to_string(VersionError error) noexcept;
```

### Free functions

```cpp
[[nodiscard]] inline expected<Version, VersionError> parse_version(std::string_view output);
```
Parse the output of `tmux -V`, with or without its trailing newline.

```cpp
[[nodiscard]] constexpr bool is_supported(const Version& version) noexcept;
```

```cpp
[[nodiscard]] std::string_view library_version() noexcept;
```
This package's own version, not tmux's.

## `libtmux/lowering.hpp`

Walk a FilterExpr and hand its shape to a caller-supplied sink.  The core stays dependency-free by never naming a serialization library: the sink is whatever the caller passes, so a JSON integration lives entirely outside this header. The same walk is what a future tmux `-f` compiler will use, which is why the node keeps its field name rather than only its accessor.

### Free functions

```cpp
[[nodiscard]] constexpr std::string_view name_of(StringOp op) noexcept;
```

```cpp
[[nodiscard]] constexpr std::string_view name_of(NumberOp op) noexcept;
```

```cpp
template <typename Sink> void replay(const LoweredExpression& nodes, Sink& sink);
```
Replay an already-lowered child into a sink.

```cpp
template <typename Entity, typename Sink> void lower(const FilterExpr<Entity>& expr, Sink& sink);
```
A sink receives one call per node in prefix order. Groups and negations are bracketed by begin/end so a sink never has to count operands itself.

```cpp
lower(operand, sink);
```

```cpp
lower(*node.operand, sink);
```

```cpp
replay(node.child, sink);
```

## `libtmux/lowered_node.hpp`

One node of a lowered expression.  Lowering flattens an expression into a sequence a caller can serialize or compile without knowing the entity type it came from. That erasure is what lets a relation keep its child: the child compares a different entity, so it cannot live in the parent's variant, but its lowered form can.

### NodeCollector

```cpp
void string_test(std::string_view field, std::string_view op, std::string_view operand);
```

```cpp
void bool_test(std::string_view field, bool expected);
```

```cpp
void number_test(std::string_view field, std::string_view op, long long operand);
```

```cpp
void begin_group(bool conjunction);
```

```cpp
void end_group();
```

```cpp
void begin_negation();
```

```cpp
void end_negation();
```

```cpp
void begin_relation(std::string_view name, int quantifier);
```

```cpp
void end_relation();
```

```cpp
[[nodiscard]] const LoweredExpression& nodes() const noexcept;
```

```cpp
[[nodiscard]] LoweredExpression take() noexcept;
```

## `libtmux/legacy_lookup.hpp`

Edge parser for the Python `field__lookup=value` spelling.  This exists so a caller migrating from Python libtmux can hand a recorded filter string straight across. It is deliberately an edge: it produces the same FilterExpr the typed fields produce, so nothing downstream knows a string was ever involved, and the typed spelling stays the only way to write a new filter.

### Free functions

```cpp
[[nodiscard]] inline expected<StringOp, LookupParseError> lookup_of(std::string_view name);
```
`eq` is the implicit lookup, matching Python's bare `field=value`. An empty name reaches here only from a key that carried no separator at all; a key ending in one is refused before this is asked.

```cpp
[[nodiscard]] expected<FilterExpr<Entity>, LookupParseError> parse_lookup(std::string_view term, std::span<const StringFieldHandle<Entity>> fields);
```

## `libtmux/expected.hpp`

The one C++23 library facility this package's public surface needs.  Recoverable failure is reported by value, not by exception, which means `std::expected`. That type arrived in C++23, so a toolchain shipping a C++20 standard library cannot provide it. Rather than fork the API, the C++20 build substitutes the reference implementation the standard type was modelled on.  Callers write `libtmux::expected` and never name either underlying type, so the same source compiles under both.

## `libtmux/abi.hpp`

Binary identity.  The C++20 and C++23 builds are not ABI-compatible: every type that carries a result differs in layout because the underlying expected differs. Linking objects from both produces a program that appears to build and then reads the wrong bytes.  An inline namespace whose name encodes the choice makes that a link error naming the missing symbol instead. Callers still write `libtmux::Server`; the namespace is inline, so it is invisible in source and decisive in the mangled name.
