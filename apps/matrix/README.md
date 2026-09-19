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
libtmux at 38a9fbb
12th Gen Intel(R) Core(TM) i7-12700H, 10 threads, linux, clang

path                       wall   processes  clients  query answer
----------------------------------------------------------------------------------------
process                   213ms          11        0  7 panes on the server [0 0 1 2 3 4 5]
chained                    16ms           1        0  7 panes on the server [0 0 1 2 3 4 5]
connection                 31ms           0        1  7 panes on the server [0 0 1 2 3 4 5]
concurrent x4              20ms           0        4  7 panes on the server [0 0 1 2 3 4 5]
chained + connection       40ms           0        1  7 panes on the server [0 0 1 2 3 4 5]
```

Add `--json` for the same results as one strict JSON document with durations
as integer nanoseconds, or `--check` to assert the claims the table makes.

## Reading it

**The last column checks equivalence.** Every measured row queries the same
six-pane topology. Only cost should differ. This is what makes the timings
comparable against the other libtmux ports.

**Count invocations, not milliseconds.** Wall clock moves with the machine. The
process column does not.

The three connection lanes run the same build through
`Server::over_control`: over one held-open control client, over a pool of four
as the Go port's `concurrent x4` does, and chained over one. Every command in
this workload is one tmux answers completely inside its guarded block —
`split-window` defers only under `-I` or `-W`, and nothing here sends either —
so none of them launches, and `--check` requires exactly zero. A lane that
fell back to launching would show its invocations there. The clients are
opened before the measured build, as the server is started before it.

The harness writes a POSIX proxy that records one invocation and execs the real
tmux, then names it twice: to the fixture as `tmux_binary`, so the server it
starts runs through it, and to the `Server` as `ExecutionPolicy::tmux_binary`,
so every measured command does too. Naming it to the fixture alone covers only
the server it starts, and every measured command then bypasses the counter.
