# Command senders

`libtmux/execution.hpp` is an optional NVIDIA stdexec adapter. Include it
explicitly and supply stdexec headers alongside the installed libtmux target.
The normal umbrella header and core CMake target do not require stdexec.

`command_sender(scheduler, server, runtime, request, stop)` submits only after
the connected sender starts on the supplied scheduler. It owns the request and
a copy of the server handle. Keep the runtime, scheduler and any explicit stop
source alive until completion. The optional stop argument is a
`stdexec::inplace_stop_token`.

The value channel carries `expected<std::string, CommandFailure>`. A failed
command or explicit cancellation remains a value with its delivery status;
allocation exceptions use the sender error channel. A receiver stop request
can stop scheduling before admission. Pass the explicit token to request
transport cancellation after admission. Cancellation cannot undo tmux work.
Global command observers still require `CommandRuntime::dispatch_ready`.

Each pending command blocks one scheduler worker. A wait and the command that
signals it need at least two available workers. `when_all` composes these
operations, but does not change the supplied scheduler's capacity. The adapter
adds no worker threads of its own.

The pinned test dependency is stdexec commit
[`34fa94719f97f49a553811423fa1200172f22a0e`](https://github.com/NVIDIA/stdexec/tree/34fa94719f97f49a553811423fa1200172f22a0e).
Tests cover lazy admission, composed replies, a wait/signal pair, failure
delivery and explicit cancellation before and after transport admission.

Configure the optional suite:

```console
$ cmake --preset cxx-dev \
    -DLIBTMUX_BUILD_EXECUTION_TESTS=ON
```

```console
$ cmake --build --preset cxx-dev \
    --target libtmux_execution_test
```

```console
$ ctest --preset cxx-dev \
    -R '^libtmux.execution$' \
    --output-on-failure \
    --no-tests=error
```

A nonblocking callback adapter remains separate work. The callback prototype
executes on GCC, while its Clang code generation exceeds the bounded checks.
This scheduler adapter establishes a portable composition path; it does not
claim callback completion or native `std::execution` availability.
