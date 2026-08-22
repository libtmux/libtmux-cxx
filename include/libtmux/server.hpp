#pragma once

// The connection root.
//
// A Server names which tmux server to talk to and how to reach it. It is a
// handle: copying one costs a reference count, and every entity taken from it
// keeps the same connection alive, so a listing outlives the Server value that
// produced it.
//
// No transport type appears in this header. `run` is declared here and defined
// against a private backend, so an async or control-mode executor can replace
// that backend without changing a caller or breaking ABI.

#include "libtmux/abi.hpp"
#include "libtmux/command.hpp"
#include "libtmux/expected.hpp"
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "libtmux/batch.hpp"
#include "libtmux/capabilities.hpp"
#include "libtmux/chain.hpp"
#include "libtmux/control.hpp"
#include "libtmux/entities.hpp"
#include "libtmux/options.hpp"
#include "libtmux/socket.hpp"
#include "libtmux/version.hpp"

LIBTMUX_NAMESPACE_BEGIN

class Server;

namespace detail {
// Declared so the private constructor has exactly one way in. Defined in a
// header this package does not install.
[[nodiscard]] Server server_over(std::shared_ptr<const Backend> backend);
} // namespace detail

class Server {
public:
  // `-S path`: the socket file, used verbatim.
  //
  // These report `CommandFailure`, the same type every other call reports,
  // rather than the `SocketError` the argument builders use: a factory that
  // failed differently is a factory nothing can be chained onto. The reason a
  // selector was rejected is in the diagnostic, and `socket_path_arguments`
  // still returns the enum for a caller that wants to branch on it.
  //
  // An observer, if given, is told about every command this server runs. It is
  // fixed at construction because the connection is immutable afterwards, and
  // that is what makes a Server safe to copy between threads. The policy is
  // fixed for the same reason, and says what a call gets when it names no
  // timeout or limit of its own.
  [[nodiscard]] static expected<Server, CommandFailure>
  at_socket_path(std::string_view path, CommandObserver observer = {},
                 ExecutionPolicy policy = {});
  // `-L name`: resolved under tmux's socket directory, as the tmux flag does.
  [[nodiscard]] static expected<Server, CommandFailure>
  at_socket_name(std::string_view name, CommandObserver observer = {},
                 ExecutionPolicy policy = {});

  // The server this process is running inside.
  //
  // tmux exports `TMUX` to everything it starts, as
  // `<socket path>,<server pid>,<session id>`. Only the socket path is read:
  // the session id is stale the moment a pane moves, and a `#()` job carries
  // no session at all — so a caller who wants the session asks tmux, rather
  // than trusting what it inherited.
  [[nodiscard]] static expected<Server, CommandFailure>
  from_env(CommandObserver observer = {}, ExecutionPolicy policy = {});

  // The server tmux would talk to with no `-L` or `-S` at all, which is the
  // one a person means when they say "my tmux".
  [[nodiscard]] static expected<Server, CommandFailure>
  at_default(CommandObserver observer = {}, ExecutionPolicy policy = {});

  // The local backend contract; no command runs. `tmux_version()` separately
  // queries the executable or the connected control server.
  [[nodiscard]] ServerCapabilities capabilities() const noexcept;

  // Run one command and return its standard output.
  //
  // The timeout still rides on the call: how long a caller will wait is a
  // property of what they asked for, and listing sessions does not share a
  // deadline with attaching a client. Unset takes the server's
  // `ExecutionPolicy`, which is thirty seconds rather than forever — a floor,
  // not a guess at what this particular command needs.
  // `output_limit` bounds how much of tmux's answer this call will hold. Past
  // it the command reports `truncated` rather than returning a prefix that
  // reads like a complete answer. Unset uses the package default, which is
  // ample for every listing and can be too small for a long scrollback.
  [[nodiscard]] expected<std::string, CommandFailure>
  run(const std::vector<std::string>& command,
      std::optional<std::chrono::milliseconds> timeout = {},
      std::optional<std::size_t> output_limit = {}) const;

  // Run several commands in one invocation. tmux runs a batch fail-fast, so a
  // failed batch is partially applied rather than rolled back, and one exit
  // status covers the group: which member failed needs control mode.
  [[nodiscard]] expected<std::string, CommandFailure>
  run_batch(const CommandBatch& batch) const;

  // Run a chain. A chain that failed validation never reaches tmux, and says
  // which step was wrong rather than surfacing a tmux message about it.
  [[nodiscard]] expected<std::string, CommandFailure>
  run_chain(const Chain& chain) const;

  // Open a control-mode connection to one session.
  //
  // This is the streaming half of the transport: a control connection stays
  // open, gives each command its own reply block, and delivers asynchronous
  // notifications between them. The synchronous surface above is unaffected —
  // a caller who never opens one never pays for it.
  //
  // Fails with `ProtocolError`, not `CommandFailure`, because that is what
  // the `Connection` it returns speaks: an error type here that the value's
  // own surface does not use would make the doorway disagree with the room.
  // `over_control` returns an ordinary `Server` and so reports the ordinary
  // failure.
  [[nodiscard]] expected<Connection, ProtocolError>
  control(std::string_view session) const;
  // The Server supplies the socket and `session` supplies the session name;
  // every other connection option is kept, including pane output policy.
  [[nodiscard]] expected<Connection, ProtocolError>
  control_with_options(std::string_view session, ConnectionOptions options) const;

  // What tmux has said on its own initiative since the last call: a window
  // renamed, a pane exited, a client attached. A Server that runs a process
  // per command hears nothing between them and answers with nothing, so a
  // caller that wants this opens one with `over_control`.
  //
  // The buffer is bounded; `dropped_notifications` says how many were
  // discarded, which distinguishes a quiet server from one that outran a
  // caller who was not collecting.
  [[nodiscard]] std::vector<Notification> take_notifications() const;
  [[nodiscard]] std::size_t dropped_notifications() const noexcept;

  // The same surface, dispatched over one open control connection instead of
  // a process per command. Entities taken from the result are ordinary
  // entities; nothing above the transport knows the difference.
  //
  // A connection carries one conversation, so commands over it are
  // serialized. Two Servers over the same socket are two conversations.
  // Live aliases must preserve the expected flag-1 reply-block count for every
  // command; otherwise use Connection's exact-count overload or subprocess.
  [[nodiscard]] expected<Server, CommandFailure>
  over_control(std::string_view session) const;
  // As above, with the connection's timeouts, limits, executable, and output
  // policy selected by the caller rather than reconstructed from a route.
  [[nodiscard]] expected<Server, CommandFailure>
  over_control_with_options(std::string_view session, ConnectionOptions options) const;

  // The subprocess backend asks `tmux -V` without touching a server; a control
  // backend asks its connected server. Both use this Server's execution policy.
  [[nodiscard]] expected<Version, CommandFailure> tmux_version() const;

  // Whether a server is answering on this socket. False covers every reason —
  // no server, no socket, a tmux that would not run — because a caller who
  // only wants to know whether to start one does not need to tell them apart.
  //
  // Bounded by default: the one call whose whole job is answering "can I reach
  // this" must not be the call that hangs. A stalled socket or a stopped
  // server answers no, in time, rather than never.
  [[nodiscard]] bool is_alive(std::chrono::milliseconds timeout = std::chrono::seconds{
                                  5}) const;
  // The same question, keeping why the answer was no.
  [[nodiscard]] expected<void, CommandFailure>
  check_alive(std::chrono::milliseconds timeout = std::chrono::seconds{5}) const;

  // End the server and everything in it.
  [[nodiscard]] expected<void, CommandFailure> kill() const;

  // One snapshot each. Iterating or filtering the result never reaches tmux
  // again; taking a current view means calling these again.
  [[nodiscard]] expected<std::vector<Session>, CommandFailure> sessions() const;
  // tmux scopes window and pane listings to the current session unless asked
  // for every one, so `-a` is part of the request rather than a caller's
  // responsibility to remember.
  [[nodiscard]] expected<std::vector<Window>, CommandFailure> windows() const;
  [[nodiscard]] expected<std::vector<Pane>, CommandFailure> panes() const;
  [[nodiscard]] expected<std::vector<Client>, CommandFailure> clients() const;

  // Block until someone signals this channel, or the deadline passes.
  //
  // tmux latches a signal: one sent while nobody is waiting satisfies the
  // next wait rather than being lost. That makes signal-before-wait safe,
  // and it also means a stale signal can release a later waiter, so a
  // channel is worth naming for one exchange rather than reusing.
  //
  // A server that dies under a waiter makes tmux exit zero, which is
  // indistinguishable from being signalled — a caller would carry on as
  // though the other side had spoken. This reports that as a failure
  // instead, which is the reason to prefer it over running the command.
  [[nodiscard]] expected<void, CommandFailure>
  wait_for(std::string_view channel,
           std::optional<std::chrono::milliseconds> timeout = {}) const;

  // Release whoever is waiting on the channel, or latch it for whoever
  // waits next.
  [[nodiscard]] expected<void, CommandFailure> signal(std::string_view channel) const;

  // Every command this tmux understands, with its alias and usage.
  //
  // Asking beats inferring: the supported-version range is a floor, not a
  // description, and a distribution can ship a build with commands left
  // out. A caller deciding whether a capability exists can look.
  [[nodiscard]] expected<std::vector<Command>, CommandFailure> commands() const;

  // The server's cut buffers, newest first as tmux orders them. A server
  // holding none answers with an empty list rather than a failure, like
  // every other listing here.
  [[nodiscard]] expected<std::vector<Buffer>, CommandFailure> buffers() const;

  // Read a file into a named buffer, and write one back out.
  //
  // The file is read and written by the tmux server, so the path is the
  // server's to resolve — which matters when it is not on this machine.
  // The bytes are not interpreted: a buffer round-tripped through a file
  // comes back identical.
  [[nodiscard]] expected<void, CommandFailure>
  load_buffer(std::string_view name, const std::filesystem::path& from) const;
  [[nodiscard]] expected<void, CommandFailure>
  save_buffer(std::string_view name, const std::filesystem::path& to) const;

  // Bind a key in a key table, and take a binding away again.
  //
  // The command is argv, not a string, so nothing here has to be quoted for
  // tmux to take it apart again correctly.
  //
  // A table name containing whitespace is refused. tmux accepts one and then
  // prints it unquoted in `list-keys`, where `-T my table X command` cannot
  // be told apart from the table `my` bound to the key `table` — so a name
  // that survives being listed is required, the same way a target refuses a
  // name that cannot survive being parsed.
  //
  // Key names are tmux's to check: unlike `send-keys`, `bind-key` reports an
  // unknown one — including an empty one — so there is nothing for this to
  // add, and nothing here repeats it.
  //
  // `repeatable` is tmux's `-r`, letting the key repeat without the prefix
  // being pressed again.
  [[nodiscard]] expected<void, CommandFailure>
  bind_key(std::string_view table, std::string_view key,
           const std::vector<std::string>& command, bool repeatable = false) const;

  // Unbinding a key that was not bound succeeds; unbinding in a table that
  // does not exist is refused. A table exists only while something is bound
  // in it, so taking away the last binding takes the table with it.
  [[nodiscard]] expected<void, CommandFailure> unbind_key(std::string_view table,
                                                          std::string_view key) const;

  // Run a shell command on the machine the server is on.
  //
  // Reports whether it ran, not what it printed. tmux hands back the output
  // on most versions and discards it on 3.3a and 3.4, and an answer that is
  // empty on two versions in the middle of the range is worse than no answer
  // at all. A caller that needs the output redirects it to a file.
  //
  // What is uniform is the exit status: a command that fails is a failure
  // here, carrying its code in `exit_code`.
  //
  // `background` is tmux's `-b`, which returns as soon as the command is
  // started. Nothing can then be said about how it ended, so a backgrounded
  // command that fails still reports success.
  [[nodiscard]] expected<void, CommandFailure> run_shell(std::string_view command,
                                                         bool background = false) const;

  // Run the commands in a file, the way tmux runs a configuration file.
  //
  // The server reads the file, so the path is the server's to resolve. A
  // file it cannot read is reported rather than passed over.
  // A control-backed Server rejects execution because the file can add an
  // unknowable number of reply blocks; `check_file` remains available there.
  [[nodiscard]] expected<void, CommandFailure>
  source_file(const std::filesystem::path& file) const;

  // Parse the same file and report what tmux would refuse in it, running
  // none of it. This is how a program checks a configuration it is about to
  // apply without applying half of it first.
  [[nodiscard]] expected<void, CommandFailure>
  check_file(const std::filesystem::path& file) const;

  // Ask tmux to expand a format, against no target but the server itself.
  //
  // Unguarded, unlike the entity forms: there is no target here that could
  // have gone away. A server kept alive with no sessions still answers its
  // own fields, and answers the session-scoped ones with nothing — which is
  // the truth about that server, not a failure to report.
  [[nodiscard]] expected<std::string, CommandFailure>
  expand(std::string_view format) const;

  // Put a message on the status line of every attached client, and send it
  // to each control client as `%message`.
  //
  // tmux expands the text as a format, so a `#{...}` in it is substituted
  // rather than shown. Text built from data belongs in `escape_literal`
  // first.
  [[nodiscard]] expected<void, CommandFailure>
  show_message(std::string_view text) const;

  // Put text in a named buffer, replacing what was there. An empty name
  // lets tmux choose one, which is what a caller copying without caring
  // about the name wants.
  [[nodiscard]] expected<void, CommandFailure> set_buffer(std::string_view name,
                                                          std::string_view data) const;

  // One object by target, for a caller holding an id or a `session:window`
  // path that came from somewhere else. The target is resolved the way tmux
  // resolves it, so a session target names that session's active pane and the
  // window that pane is in. A target tmux cannot resolve is reported missing.
  [[nodiscard]] expected<Session, CommandFailure>
  session(std::string_view target) const;
  [[nodiscard]] expected<Window, CommandFailure> window(std::string_view target) const;
  [[nodiscard]] expected<Pane, CommandFailure> pane(std::string_view target) const;

  // Created detached, and returned, because tmux prints what it made. Windows
  // psmux rejects typed creation: concurrent creators cannot prove ownership.
  [[nodiscard]] expected<Session, CommandFailure>
  new_session(std::string_view name) const;
  [[nodiscard]] expected<Session, CommandFailure>
  new_session(NewSessionOptions options) const;

  [[nodiscard]] expected<std::vector<OptionEntry>, CommandFailure>
  options(std::string_view target = {}) const;
  // The server's own options, which are neither session nor window options
  // and are the only ones a server without a session still has.
  [[nodiscard]] expected<std::vector<OptionEntry>, CommandFailure>
  server_options() const;
  [[nodiscard]] expected<void, CommandFailure>
  set_server_option(std::string_view name, std::string_view value) const;
  [[nodiscard]] expected<std::vector<OptionEntry>, CommandFailure>
  global_options() const;
  // Sets the value every session inherits, rather than one session's own.
  [[nodiscard]] expected<void, CommandFailure>
  set_global_option(std::string_view name, std::string_view value) const;
  [[nodiscard]] expected<std::vector<OptionEntry>, CommandFailure>
  hooks(std::string_view target = {}) const;
  // A hook set globally is not reported by the unscoped listing, so reading it
  // back needs the scope it was set with.
  [[nodiscard]] expected<std::vector<OptionEntry>, CommandFailure> global_hooks() const;
  [[nodiscard]] expected<void, CommandFailure>
  set_global_hook(std::string_view name, std::string_view command) const;

private:
  explicit Server(std::shared_ptr<const detail::Backend> backend)
      : backend_{std::move(backend)} {}

  friend Server detail::server_over(std::shared_ptr<const detail::Backend> backend);

  [[nodiscard]] expected<std::vector<OptionEntry>, CommandFailure>
  show(std::vector<std::string> request, std::string_view target) const;

  std::shared_ptr<const detail::Backend> backend_;
};

LIBTMUX_NAMESPACE_END
