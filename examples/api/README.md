# Complete API programs

Each program is a complete POSIX C++ application with imports, an entry point,
checked errors, and cleanup. They use the public `libtmux::testing` fixture to
own a private tmux server. Application code uses `libtmux::Server` to address
its server; the fixture only supplies the isolated daemon for these examples.

Needs clang with libc++, CMake 3.25 or newer, Ninja and tmux 3.3a or newer. The
fixture requires POSIX; the Windows psmux preview is not covered.

| Program | Behavior |
| --- | --- |
| [server.cpp](server.cpp) | Construct and copy a handle for an explicit socket. |
| [listings.cpp](listings.cpp) | Read sessions, windows, panes, and clients. |
| [relations.cpp](relations.cpp) | Traverse children and check their owners. |
| [new-session.cpp](new-session.cpp) | Create a detached session with a named first window. |
| [new-window-pane.cpp](new-window-pane.cpp) | Create a window and split a pane without moving window selection. |
| [query.cpp](query.cpp) | Filter snapshots and distinguish missing from ambiguous matches. |
| [input-capture.cpp](input-capture.cpp) | Wait for readiness, send text, and capture confirmed output. |

## Build a standalone consumer

From the repository root, install the library and its public fixture:

```console
$ cmake -S . -B build/api-package -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/clang-libcxx.cmake" \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=Release \
    -DLIBTMUX_BUILD_TESTS=OFF \
    -DLIBTMUX_BUILD_EXAMPLES=OFF \
    -DLIBTMUX_BUILD_TESTING_LIBRARY=ON \
    && cmake --build build/api-package --parallel 2 \
    && cmake --install build/api-package --prefix "$PWD/build/api-prefix"
```

Choose one program and copy the complete file and its project into a separate
consumer directory:

```console
$ mkdir -p build/api-consumer \
    && cp examples/api/project/CMakeLists.txt build/api-consumer/CMakeLists.txt \
    && cp examples/api/input-capture.cpp build/api-consumer/main.cpp
```

Build against the installed package, then run it:

```console
$ cmake -S build/api-consumer -B build/api-consumer/out -G Ninja \
    -DCMAKE_PREFIX_PATH="$PWD/build/api-prefix" \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_CXX_FLAGS=-stdlib=libc++ \
    && cmake --build build/api-consumer/out --parallel 2 \
    && ./build/api-consumer/out/api_example
```

The capture program prints `got:hello C++`.

## Verification

CTest runs these same files:

```console
$ ctest --preset cxx-dev -R '^example\.api\.' --output-on-failure
```

To write a JSON report of each run:

```console
$ uv run scripts/docs/check_api_examples.py \
    --binary-dir build/cxx-dev/examples/api \
    --report build/api-example-results.json
```

[`api-examples.json`](api-examples.json) maps each program to its API
declarations and expected output.
