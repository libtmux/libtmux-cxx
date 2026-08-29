#pragma once

// The private half of the transport seam.
//
// Everything that reaches a process lives behind this interface. `Server` and
// `Snapshot` hold one by shared pointer and neither exposes it, so an async or
// control-mode executor arrives as another implementation rather than as a
// change to any installed header.
//
// Dispatch is virtual, which the transport bakeoff measured at about 19ns
// against a command that launches a process. That is the cost of the seam
// being real rather than asserted.

#include "libtmux/abi.hpp"
#include "libtmux/batch.hpp"
#include "libtmux/capabilities.hpp"
#include "libtmux/command.hpp"
#include "libtmux/control.hpp"
#include "libtmux/expected.hpp"
#include "libtmux/version.hpp"
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

LIBTMUX_NAMESPACE_BEGIN

class Server;

namespace detail {

// The command as one line, which is what a person reads in a log or in a
// diagnostic. Bounded: a format request is long, and a caller wanting all of
// it has the argv already.
[[nodiscard]] std::string rendered_command(const CommandRequest& command);

class Backend {
public:
  Backend() = default;
  explicit Backend(CommandObserver observer, ExecutionPolicy policy = {})
      : observer_{std::move(observer)}, policy_{policy} {}
  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;
  Backend(Backend&&) = delete;
  Backend& operator=(Backend&&) = delete;
  virtual ~Backend() = default;

  // No default argument: a default on a virtual function binds to the static
  // type, so an override could silently disagree about what "no timeout" is.
  [[nodiscard]] virtual expected<std::string, CommandFailure>
  run(const CommandRequest& command, std::optional<std::chrono::milliseconds> timeout,
      std::optional<std::size_t> output_limit) const = 0;

  // Route an entity command through its owning psmux session. POSIX backends
  // already have one server-wide namespace, so their ordinary run is exact.
  [[nodiscard]] virtual expected<std::string, CommandFailure>
  run_in_session(const CommandRequest& command, std::string_view session_id,
                 std::string_view session_name,
                 std::optional<std::chrono::milliseconds> timeout,
                 std::optional<std::size_t> output_limit) const {
    static_cast<void>(session_id);
    static_cast<void>(session_name);
    return run(command, timeout, output_limit);
  }

  [[nodiscard]] virtual expected<bool, CommandFailure>
  session_belongs(std::string_view session_id, std::string_view session_name,
                  std::optional<std::chrono::milliseconds> timeout) const {
    static_cast<void>(session_id);
    static_cast<void>(session_name);
    static_cast<void>(timeout);
    return true;
  }

  [[nodiscard]] expected<std::string, CommandFailure>
  run(const CommandRequest& command) const {
    return run(command, std::nullopt, std::nullopt);
  }

  // A batch, with its structure intact.
  //
  // Flattening it to one argv is right for a transport that execs directly:
  // tmux reads a bare `;` element as the separator. It is wrong for control
  // mode, which writes a line and escapes every byte of every argument, so a
  // flattened separator arrives as a literal semicolon and the whole batch
  // collapses into one command with the rest as junk arguments. That reported
  // success, which is why the transport is asked rather than told.
  [[nodiscard]] virtual expected<std::string, CommandFailure>
  run_batch(const CommandBatch& batch, std::optional<std::chrono::milliseconds> timeout,
            std::optional<std::size_t> output_limit) const {
    return run(batch.request(), timeout, output_limit);
  }

  // The `-L name` or `-S path` pair that selects the server.
  [[nodiscard]] virtual const std::vector<std::string>& connection() const noexcept = 0;

  // Which server this talks to: a resolved socket path on POSIX and a logical
  // psmux selector on Windows. Empty identifies no server, even itself.
  //
  // On POSIX the selector cannot serve here: `-L work` and `-S <its path>`
  // select one server and must compare as one.
  [[nodiscard]] virtual std::string_view identity() const noexcept { return {}; }

  // Unknown custom executors fail capability checks closed. Concrete package
  // backends override this with the routing contract they implement.
  [[nodiscard]] virtual ServerCapabilities capabilities() const noexcept { return {}; }

  // Which tmux is behind this connection. How to ask depends on the executor,
  // so the executor answers.
  [[nodiscard]] virtual expected<Version, CommandFailure> version() const = 0;

  // What tmux has said on its own initiative since the last call. A transport
  // that runs a process per command hears nothing between them and answers
  // with nothing; a connection that stays open hears everything.
  [[nodiscard]] virtual std::vector<Notification> take_notifications() const {
    return {};
  }
  [[nodiscard]] virtual std::size_t dropped_notifications() const noexcept { return 0; }

  // A synchronous tmux command inserted by this one writes to subprocess
  // stdout, but control mode frames it as the next reply block.
  [[nodiscard]] virtual expected<std::string, CommandFailure>
  run_inserted(const CommandRequest& command,
               std::optional<std::chrono::milliseconds> timeout,
               std::optional<std::size_t> output_limit) const {
    return run(command, timeout, output_limit);
  }

  // The observer this backend was built with, so a Server that opens a
  // connection over the same selector keeps watching the same way.
  [[nodiscard]] const CommandObserver& observer() const noexcept { return observer_; }

  // What a call that names no timeout of its own gets. Applied where a caller
  // reaches the library, not here: this layer keeps taking `nullopt` to mean
  // no deadline, which is what `wait_for` needs to be able to ask for.
  [[nodiscard]] const ExecutionPolicy& policy() const noexcept { return policy_; }

protected:
  [[nodiscard]] expected<std::string, CommandFailure>
  report_failure(const CommandRequest& command, CommandFailure failure) const;

  // Render a command the way tmux received it and hand it to the observer, if
  // there is one. Called after the command finishes and outside any lock.
  void observe(const CommandRequest& command, const CommandFailure* failure) const;

private:
  [[nodiscard]] CommandFailure redact(CommandFailure failure,
                                      const CommandRequest& command) const;

  CommandObserver observer_;
  ExecutionPolicy policy_;
};

// Do two backends talk to one tmux?
//
// The pointer first, because a value and its refresh share a backend and that
// answer costs nothing. Then the socket, so two `Server`s opened on one socket
// agree — which is what a caller means by the same server, and what a command
// combining two entities has to be able to ask.
[[nodiscard]] inline bool same_server(const Backend* left,
                                      const Backend* right) noexcept {
  if (left == right) {
    return true;
  }
  if (left == nullptr || right == nullptr) {
    return false;
  }
  return !left->identity().empty() && left->identity() == right->identity();
}

// tmux in a child process: the only executor this library binds to.
class SubprocessBackend final : public Backend {
public:
  explicit SubprocessBackend(std::vector<std::string> connection,
                             CommandObserver observer = {},
                             ExecutionPolicy policy = {});

  // Declaring an override hides the base's other overload, and the
  // one-argument form is how most callers spell "no timeout".
  using Backend::run;

  [[nodiscard]] expected<std::string, CommandFailure>
  run(const CommandRequest& command, std::optional<std::chrono::milliseconds> timeout,
      std::optional<std::size_t> output_limit) const override;

  [[nodiscard]] expected<std::string, CommandFailure>
  run_in_session(const CommandRequest& command, std::string_view session_id,
                 std::string_view session_name,
                 std::optional<std::chrono::milliseconds> timeout,
                 std::optional<std::size_t> output_limit) const override;

  [[nodiscard]] expected<bool, CommandFailure>
  session_belongs(std::string_view session_id, std::string_view session_name,
                  std::optional<std::chrono::milliseconds> timeout) const override;

  [[nodiscard]] const std::vector<std::string>& connection() const noexcept override {
    return connection_;
  }

  [[nodiscard]] std::string_view identity() const noexcept override {
    return identity_;
  }

  [[nodiscard]] ServerCapabilities capabilities() const noexcept override {
#if defined(_WIN32)
    return {.implementation = ServerImplementation::psmux,
            .backend = BackendKind::subprocess};
#else
    return {.implementation = ServerImplementation::tmux,
            .backend = BackendKind::subprocess};
#endif
  }

  // `tmux -V` answers without connecting, so this works against a socket with
  // no server on it.
  [[nodiscard]] expected<Version, CommandFailure> version() const override;

private:
  [[nodiscard]] expected<std::string, CommandFailure>
  run_scoped(const CommandRequest& command, std::optional<std::string_view> session,
             std::optional<std::chrono::milliseconds> timeout,
             std::optional<std::size_t> output_limit) const;

  std::vector<std::string> connection_;
  // Resolved once, at construction: the answer depends on `TMUX_TMPDIR` and on
  // the filesystem, and a server that changed which tmux it meant partway
  // through its life would be worse than one that cannot say.
  std::string identity_;
};

// Build a Server over any backend. The only way to reach the private
// constructor, and the reason the seam can be exercised without a tmux.
[[nodiscard]] Server server_over(std::shared_ptr<const Backend> backend);

} // namespace detail
LIBTMUX_NAMESPACE_END
