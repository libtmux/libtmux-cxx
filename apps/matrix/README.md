# matrix

What each way of reaching tmux costs, printed as one table.

```console
$ cmake -S . -B build/matrix -G Ninja \
    --toolchain cmake/toolchains/clang-libcxx.cmake \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=Release \
    -DLIBTMUX_BUILD_TESTS=OFF \
    -DLIBTMUX_BUILD_MCP_SERVER=OFF \
    -DLIBTMUX_BUILD_EXAMPLES=OFF \
    -DLIBTMUX_BUILD_BENCHMARKS=ON \
    -DLIBTMUX_BUILD_TESTING_LIBRARY=ON \
    -DLIBTMUX_FETCH_DEPS=ON
```

```console
$ cmake --build build/matrix --target libtmux_matrix
```

```console
$ ./build/matrix/apps/matrix/libtmux_matrix
```

The compilers are named because the toolchain adds `-stdlib=libc++`, which
GCC rejects: left to itself CMake can pick the system `c++` and stop at its
compiler check.

```

building a 6-pane window, tmux 3.7d
libtmux at a92937b
12th Gen Intel(R) Core(TM) i7-12700H, 10 threads, linux, clang

path                       wall   processes  clients  query answer
----------------------------------------------------------------------------------------
process                    26ms          11        0  7 panes on the server [0 0 1 2 3 4 5]
chained                     5ms           1        0  7 panes on the server [0 0 1 2 3 4 5]
connection                    -           -        -  unimplemented: libtmux::control::Connection carries notifications and pane-output policy; it exposes no method that dispatches a command
concurrent x4                 -           -        -  unimplemented: requires a dispatching control connection
chained + connection          -           -        -  unimplemented: requires a dispatching control connection
```

Add `--json` for the same results as one strict JSON document with durations
as integer nanoseconds, or `--check` to assert the claims the table makes.

## Reading it

**The last column checks equivalence.** Every measured row queries the same
six-pane topology. Only cost should differ. This is what makes the timings
comparable against the other libtmux ports.

**Count invocations, not milliseconds.** Wall clock moves with the machine. The
process column does not.

Three lanes are reported as unimplemented rather than left out. An earlier
version of this note said tmux itself forbade them, because `split-window` is
one of the twelve commands that can return `CMD_RETURN_WAIT`. That overstated
it: `split-window` defers only under `-I` or `-W`, and this workload sends
neither, so its guarded block is the whole answer. The lanes are unbuilt, not
unbuildable.

The distinction matters because it is narrow. No `list-*` command can defer,
and `display-message` defers only under `-I`, so a listing *is* answered
completely by its guarded block — which is why `tests/executor_seam_test.cpp`
can serve every listing over one held-open connection and match this table's
launching path row for row. A benchmark shaped like that workload would show
what the control path buys; this one, built out of mutations, would not.
libtmux-ts makes the same choice; libtmux-go, libtmux-rs, Swift, .NET and Java
dispatch commands over their connections and measure that.

The harness writes a POSIX proxy that records one invocation and execs the real
tmux, then names it twice: to the fixture as `tmux_binary`, so the server it
starts runs through it, and to the `Server` as `ExecutionPolicy::tmux_binary`,
so every measured command does too. Naming it to the fixture alone covers only
the server it starts, and every measured command then bypasses the counter.
