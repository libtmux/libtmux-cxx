# Ownership and executable examples

## Automatic examples and test fixtures

An ordinary program includes its headers and uses `Server::at_default()`. The test harness supplies `LIBTMUX_SOCKET_PATH` or `LIBTMUX_SOCKET_NAME` in the child's environment. The program has no fixture setup. `ScopedTmuxServer` starts and owns the test daemon, waits for its exit, then removes the fixture directory; an unverified exit retains the directory and reports the failure.

This complete program is [08-lifecycle.cpp](../examples/08-lifecycle.cpp). The example harness compiles and runs it against a private endpoint. The quote check compares the displayed source with that file. It creates a session, creates and reuses a window, adopts and destroys a pane, then lists servers in the selected socket directory. Checked cleanup removes the created window and session.

```cpp
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

#include <libtmux/libtmux.hpp>

libtmux::CommandFailure report(const libtmux::LifecycleFailure& failure) {
  if (failure.rollback)
    std::cerr << "rollback: " << failure.rollback->diagnostic << '\n';
  return failure.primary;
}

int main() {
  auto server = libtmux::Server::at_default();
  if (!server) {
    std::cerr << server.error().diagnostic << '\n';
    return 1;
  }
  auto work = libtmux::own_session(
      *server, {.name = "libtmux-lifecycle-" + std::to_string(::getpid())});
  if (!work) {
    std::cerr << work.error().primary.diagnostic << '\n';
    if (work.error().rollback)
      std::cerr << work.error().rollback->diagnostic << '\n';
    return 1;
  }
  const auto result = libtmux::with_owned(
      *work,
      [&](const libtmux::Session& session)
          -> libtmux::expected<void, libtmux::CommandFailure> {
        auto build = libtmux::find_or_create_window(session, {.name = "build"});
        if (!build)
          return libtmux::unexpected(report(build.error()));
        auto reused = libtmux::find_or_create_window(session, {.name = "build"});
        if (!reused)
          return libtmux::unexpected(report(reused.error()));
        std::cout << "window created=" << build->created()
                  << " reused=" << !reused->created() << '\n';
        auto first = build->value.active_pane();
        if (!first)
          return libtmux::unexpected(first.error());
        auto borrowed = first->split();
        if (!borrowed)
          return libtmux::unexpected(borrowed.error());
        auto adopted = libtmux::adopt(*borrowed);
        if (!adopted)
          return libtmux::unexpected(report(adopted.error()));
        if (auto closed = adopted->close(); !closed)
          return closed;
        std::cout << "adopted pane cleaned=" << adopted->cleanup().report().complete
                  << '\n';
        const auto discovery = libtmux::discover_servers(
            {.roots = {std::filesystem::path{server->socket_path()}.parent_path()},
             .configured_roots = false});
        if (!discovery)
          return libtmux::unexpected(discovery.error());
        std::cout << discovery->servers.size()
                  << " server(s) in the selected directory\n";
        for (const auto& diagnostic : discovery->diagnostics) {
          if (diagnostic.failure)
            std::cerr << diagnostic.path << ": " << diagnostic.failure->diagnostic
                      << '\n';
        }
        if (build->owner)
          return build->owner->close();
        return {};
      });
  if (result.body_failure)
    std::cerr << result.body_failure->diagnostic << '\n';
  if (result.cleanup.failure)
    std::cerr << result.cleanup.failure->diagnostic << '\n';
  if (result.body_exception || result.cleanup.exception)
    std::cerr << "body or cleanup threw\n";
  return result.ok() ? 0 : 1;
}
```

## Taking ownership of existing objects

Call `adopt(server)`, `adopt(session)`, `adopt(window)` or `adopt(pane)` to accept destruction responsibility. `adopt` returns `expected<Owned<T>, LifecycleFailure>` and sends no creation command. Reading or copying a borrowed value does not destroy it at scope exit. The program above adopts the `Pane` returned by `split()`.

Each owner retains the server's private hard-link route, daemon PID, start time, generation token and exact object ID. A renamed session or moved pane keeps that ID. A window owner destroys the window, its links and panes. Server adoption destroys the entire accepted daemon; use an explicitly disposable endpoint for that operation.

Adoption initializes the reserved server option `@libtmux_owner_generation` when it is absent. An existing value must contain exactly 32 ASCII hexadecimal characters. The owner retains a valid value and refuses an empty or malformed one. Do not shadow or change this option. Cleanup checks the accepted identity inside the daemon executing the destructive command.

## Finding running tmux servers

`discover_servers(DiscoveryOptions)` scans the supplied directories without recursion. With `configured_roots=true`, it also considers the captured nonempty `TMUX_TMPDIR/tmux-<uid>` and `/tmp/tmux-<uid>`. The example passes the selected server's parent directory and disables additional roots, so its output describes that directory.

The defaults bound discovery to 16 roots, 256 directory entries, 64 probes, a two-second elapsed budget and 100 milliseconds per probe. `servers` contains successful socket paths and daemon identities. `diagnostics` records opened or inaccessible roots, duplicate roots and sockets, non-sockets, failed probes and limits; `truncated` distinguishes a stopped search from a complete scan. Symlinks follow filesystem semantics, including `symlink/..`. Duplicate roots consume the root bound. Probes use a command that cannot start a daemon. A blocked filesystem metadata call can exceed the elapsed budget.

## Find or create

`find_or_create_server(startable, first_session)` reuses the one daemon at that endpoint or starts one through the supplied startable handle. A random inherited startup nonce proves which caller started it. If another client wins startup, cleanup may remove this call's new session but never claims its daemon.

`find_or_create_session(server, creation)` matches the exact session name. `find_or_create_window(session, creation)` matches a window name within that session. `find_or_create_pane(window, key, creation)` matches the nonempty application key in `@libtmux_pane_key` within that window. Window names and pane keys can match multiple objects; those calls return a validation failure. tmux enforces unique session names, and an endpoint addresses one daemon.

Each result exposes `value`, `created()` and an optional `owner`. Reuse remains borrowed. A created owner lives as long as the result unless the caller moves it elsewhere. The library serializes these calls within this process, even across separate handles. Cancellation and timeout apply while waiting for that reservation. Other tmux clients can change state during the operation; this is not a transaction across processes. Observer reentry returns `overloaded` instead of waiting for its own acquisition.

## Cleanup at scope exit

`Owned<T>` is move-only. Its destructor attempts cleanup without throwing. Call `close()` for a checked result, or retain `cleanup()` before scope exit to inspect the latest attempt and retry. A successful close is idempotent. A failed close remains incomplete. `CleanupHandle::report()` records completion, an in-progress attempt, the attempt count, a command failure or an exception. Copies of the handle share the result; destroying the handle itself performs no cleanup.

`with_owned(owner, body)` runs an expected-style body, catches an exception from it and attempts checked cleanup. `OwnedScopeResult` retains `body_failure`, `body_exception` and the cleanup report separately. The program above prints the body and cleanup diagnostics and returns a failure status if either operation fails. Command observers run without lifecycle mutexes held; inspecting a cleanup report is supported, and recursively closing the same active owner returns `overloaded`.

`LifecycleOptions` supplies acquisition and cleanup timeouts. Its cancellation predicate must be nonthrowing and thread-safe; an atomic flag works on both supported standard-library lanes, and a caller can bind `std::stop_token` where their standard library supplies it. Cleanup uses a separate timeout and ignores acquisition cancellation. C++23 uses `std::expected`; the C++20 build retains its existing `tl::expected` configuration.

Owned creation prints an ASCII receipt from the creating daemon, retains it before interpreting command failure, then constructs the returned snapshot. A valid receipt allows rollback after a command refusal, timeout, cancellation or snapshot failure. `LifecycleFailure` retains the primary error, optional rollback error, receipt and exception. Its `cleanup` handle retains the daemon-bound transport, records the rollback attempt and lets you retry after resolving a cleanup failure. Without a complete receipt, `uncertain` marks an unknown dispatch result; the library does not guess an object from its name. Allocation failure and process termination can still interrupt acquisition; an external test runner must retain its own daemon responsibility.

An ownership refusal can accompany a command failure. If a creation receipt contradicts the accepted daemon identity, or server startup reaches a daemon started by another client, `primary` describes that ownership problem and `command_failure` retains the captured command's original kind, delivery status, exit code and diagnostic. `exception` retains the original observer exception. A contradictory receipt sets `uncertain`, retains the receipt and grants no cleanup handle. A foreign startup permits rollback of this call's new session; the existing daemon and its other sessions remain borrowed. `command_failure` is empty when neither the command nor its observer failed. On other failure paths, the captured command failure remains in `primary`.

## Environment defaults and overrides

Explicit socket factories override environment defaults. `Server::at_default()` selects the first nonempty `LIBTMUX_SOCKET_PATH`, `LIBTMUX_SOCKET_NAME`, or valid `TMUX` context, then the named default socket. The factory captures the endpoint and child environment. Later host edits cannot redirect operations or cleanup. There is no `LIBTMUX_SOCKET_ENV` variable.

`ExecutionPolicy::child_environment` supplies a complete child environment without changing the host. Its absence captures the host once. `TMUX` and `TMUX_PANE` are removed from subprocess launches after endpoint selection. Named defaults use the captured absolute `TMUX_TMPDIR`, or `/tmp`; invalid or removed roots fail without fallback. tmux server/session environment setters change tmux state rather than the host process.

## Sandbox and example testing

The repository harness checks normal execution, body failure and cleanup failure around the unchanged ordinary program. Lifecycle tests use individually owned `ScopedTmuxServer` fixtures, exercise all four owners, roll back failed receipts and replace a real daemon while forcing numeric identity comparisons to match. The generation check must preserve the replacement.

The Markdown quote checker verifies the displayed program against its compiled source. This integration does not implement Astro Markdown/MDX, Sphinx reStructuredText/MyST or Python doctest collection. Those format adapters, expected-output/group semantics and outer recovery after a crashed example runner remain separate work. RAII does not execute after SIGKILL or an abrupt process crash.

The new ownership surface requires the POSIX subprocess tmux backend. Unsupported transports return `FailureKind::unsupported` before dispatch. These additions leave existing borrowed APIs, public documentation and platform-specific compatibility paths in place.
