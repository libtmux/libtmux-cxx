# Bounded asynchronous commands

The contract of [`CommandRuntime`][ref-commandruntime] and
[`CommandOperation`][ref-commandoperation], which the
[README](../README.md#bounded-asynchronous-commands) summarises in a few
bullets: how a command is admitted, observed, waited on, cancelled and retired,
and what shutdown promises. The reference pages for each type are on
[libtmux.org][ref-index].

## A complete program

```cpp
// Given: const libtmux::Server& server
std::size_t observed = 0U;
auto async_server = libtmux::Server::at_socket_path(
    server.socket_path(), [&observed](const libtmux::CommandReport&) { ++observed; });
if (!async_server.has_value()) {
  std::cerr << std::format("{}\n", async_server.error());
  return 1;
}

auto started_runtime =
    libtmux::CommandRuntime::start(libtmux::CommandRuntimeConfig{.capacity = 1U});
if (!started_runtime.has_value()) {
  std::cerr << std::format("{}\n", started_runtime.error());
  return 1;
}
auto runtime = *std::move(started_runtime);
auto submitted =
    async_server->try_submit(runtime, {"display-message", "-p", "async result"});
if (!submitted.has_value()) { // Refused before admission.
  std::cerr << std::format("{}\n", submitted.error());
  return 1;
}
const auto ready = submitted->wait_for(std::chrono::seconds{5});
if (!ready.has_value() || !*ready) {
  // A wait timeout keeps the command alive. Cancellation is a separate choice.
  static_cast<void>(submitted->request_cancel());
}
auto result = std::move(*submitted).wait();
if (!result.has_value()) { // Failed after admission.
  std::cerr << std::format("{}\n", result.error());
  return 1;
}
if (runtime.wait_ready_for(std::chrono::seconds{5}) != libtmux::ReadyStatus::ready) {
  return 1;
}

const auto held = runtime.snapshot();
std::cout << std::format("{}/{} slot(s), {} observation(s) pending\n", held.in_flight,
                         held.capacity, held.pending_observers);
std::cout << std::format("dispatched {} observation(s)\n", runtime.dispatch_ready());

auto detached =
    async_server->try_submit(runtime, {"display-message", "-p", "detached"});
if (!detached.has_value()) {
  std::cerr << std::format("{}\n", detached.error());
  return 1;
}
std::move(*detached).detach(); // Keep no result; the observation remains.
if (runtime.wait_ready_for(std::chrono::seconds{5}) != libtmux::ReadyStatus::ready) {
  return 1;
}
std::cout << std::format("discarded {} observation(s)\n", runtime.discard_ready());

const auto shutdown = runtime.close();
if (shutdown.failure.has_value()) {
  std::cerr << std::format("{}\n", *shutdown.failure);
  return 1;
}
std::cout << std::format("runtime stopped: {}; safe to unload: {}; observed: {}\n",
                         shutdown.transports_stopped, shutdown.safe_to_unload,
                         observed);
if (!shutdown.transports_stopped || !shutdown.safe_to_unload || observed != 1U) {
  return 1;
}
```

## Starting a runtime

No runtime is hidden in a `Server`.
[`CommandRuntime::start`][ref-commandruntime-start] creates the
transport threads and fixes their admission capacity; zero or a startup
failure returns `unexpected` with `delivery == not_started`. A full runtime
refuses rather than waiting for a slot.

`Server::try_submit` asks the daemon for its release before admission, so a
submission to an endpoint whose daemon has not answered yet can block the
calling thread for as long as that probe takes, up to the command's timeout. A
live daemon's answer is remembered; an absent endpoint is asked again on every
submission. Admission itself never waits for a slot.

## Admission

`Server::try_submit` also returns an outer `unexpected` before admission for a
moved-from or stopped runtime, an invalid or empty command, a daemon below the
floor, an unsupported custom backend, a `Server` opened before its socket
existed, a full capacity, or a failure to register its observer. These create
no operation or observer record; refusals against a live runtime increment
`refused`, while a moved-from runtime has no counters. They are safe to retry
when their cause is fixed. Once it
returns a [`CommandOperation`][ref-commandoperation], every tmux, transport, cancellation, or runtime
publication failure is that operation's eventual result. A server that dies
after its handle captured the socket can therefore be admitted and fail through
`wait`; its [`DeliveryStatus`][ref-deliverystatus] says what the transport can prove. The runtime also
preserves its first lifecycle failure in `close().failure`, the only channel
available when no operation can carry it.

## Observers

Only commands accepted by `try_submit` enter a `Server`'s global observer.
Completion enqueues the record but invokes nothing: `wait`, destruction, and
`detach` leave it for the runtime. `dispatch_ready` invokes one snapshot of
ready records on its caller's thread; `discard_ready` releases ready records
without callbacks. Only one dispatch or discard call runs at once, so a
competing caller returns zero. If a callback throws, its record is already
released, the exception propagates, and later ready records remain queued.

## Capacity

One capacity slot remains charged until three independent legs end: the
transport retires, the operation is waited, detached, or destroyed, and its
observer is claimed for dispatch or discarded. A callback no longer holds the
slot while it runs, though it still prevents safe module unload. A command with
no observer starts with that leg complete. Admission is serialised, and
accepted commands enter the transport in admission order; completion order is
not fixed, and callers racing from different threads have no priority
guarantee.

## Waiting and cancelling

`CommandOperation::wait_for` and `wait_until` observe readiness without consuming
the handle. Their `false` result means the wait expired: the command keeps running
and retains its runtime slot. The timeout passed to `try_submit` instead bounds
the command itself, whose eventual result reports
[`FailureKind::timeout`][ref-failurekind].
`request_cancel` separately asks the transport to withdraw the command; its
eventual `DeliveryStatus` still says whether tmux may have acted.

When the standard library defines `__cpp_lib_jthread`, the operation's `wait`,
`wait_for` and `wait_until` also accept `std::stop_token`. A stop request invokes
the same transport cancellation while that wait is active. A completed result
still wins over a later stop request. libc++ 18 requires an experimental library
mode for stop tokens; the ordinary pinned build keeps `request_cancel` available
and does not enable that mode implicitly.

## Waiting for observers

`wait_ready` and `wait_ready_for` block until an observer is ready, their
deadline expires, or `close` finishes with no ready observations left. They
return [`ReadyStatus::ready`][ref-readystatus], `timeout` or `closed` and never
run callbacks. A ready observation takes precedence over an expired deadline or
closure; callers still choose `dispatch_ready` or `discard_ready`.

## Counters

`snapshot` is one lock-consistent instant; it does not wait for command
completion or runtime work to finish. `accepted`, `refused`, and `completed`
are cumulative; `completed` means the result was published and its observer
record, when present, became ready. `in_flight` counts commands retaining any
of the three legs. The two `pending_*` fields count result and observer
obligations whether or not their data is ready. `accepting` says stop or close
has not ended admission; it does not promise that capacity or preflight will
accept the next command.

## Shutdown

`request_stop` stops admission and requests cancellation without joining.
`close` blocks until runtime threads stop, invokes and discards no observer,
and returns the same cached report to every concurrent or later caller. A
throw before a successful report leaves `close` retryable. Because the report
never refreshes, discharge result and observer obligations before the first
successful `close` when `safe_to_unload` must be true. That flag means the
transports stopped, both pending counts reached zero, and no caller-side
`dispatch_ready` or `discard_ready` call remained active, including observer
callback-target teardown. It does not mean every command succeeded, and the
caller must prevent later runtime use before unloading code.

Destroying a runtime stops and joins its threads, discards pending observer
records without invoking them, and exposes no report. A `CommandOperation` may
outlive the runtime, but it retains no runtime thread. Use explicit `close`
when shutdown or module-unload status matters.

## Where it runs

The ordinary Linux test presets execute this runtime. macOS and the
C++20/C++23 experimental Windows path against pinned psmux are supported on a
best-effort basis, and CI coverage for them is highly limited. Windows retains
the narrower typed-operation limits described in
[Windows through psmux](windows.md).

## Child process descriptors

POSIX child launches do not inherit unrelated descriptors. glibc 2.34 and
newer use native close-from actions, and macOS uses CLOEXEC-default. Other
Linux builds use numeric close actions; that fallback assumes no concurrent
privileged increase of the hard descriptor limit. Ordinary concurrent
allocation and soft-limit changes remain covered.

[ref-commandoperation]: https://libtmux.org/en/cxx/latest/reference/libtmux-commandoperation/
[ref-commandruntime]: https://libtmux.org/en/cxx/latest/reference/libtmux-commandruntime/
[ref-commandruntime-start]: https://libtmux.org/en/cxx/latest/reference/libtmux-commandruntime-start/
[ref-deliverystatus]: https://libtmux.org/en/cxx/latest/reference/libtmux-deliverystatus/
[ref-failurekind]: https://libtmux.org/en/cxx/latest/reference/libtmux-failurekind/
[ref-index]: https://libtmux.org/en/cxx/latest/reference/
[ref-readystatus]: https://libtmux.org/en/cxx/latest/reference/libtmux-readystatus/
