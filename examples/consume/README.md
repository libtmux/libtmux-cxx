# Consuming the installed package

The shortest complete answer to "what do I put in my `CMakeLists.txt`". Two
files, one of them three lines long:

```cmake
find_package(libtmux REQUIRED)
target_link_libraries(your_target PRIVATE libtmux::libtmux)
```

That is the whole integration. The library has no dependencies, so nothing else
has to be found, fetched or forwarded.

## Running it

Install the library somewhere, then build this against it:

### POSIX

```console
$ cmake -S ../.. -B /tmp/libtmux-build -DLIBTMUX_BUILD_TESTS=OFF -DLIBTMUX_BUILD_EXAMPLES=OFF
```

```console
$ cmake --build /tmp/libtmux-build && cmake --install /tmp/libtmux-build --prefix /tmp/libtmux-prefix
```

```console
$ cmake -S . -B /tmp/consume-build -DCMAKE_PREFIX_PATH=/tmp/libtmux-prefix && cmake --build /tmp/consume-build
```

```console
$ /tmp/consume-build/consume
```

It prints `libtmux <version> consumed` and exits zero.

### Windows

From a Visual Studio 2022 Developer PowerShell in this directory, build and
install the library without its psmux-dependent smoke, then consume only the
installed package:

```console
$ cmake -S ../.. -B build/libtmux -G "Visual Studio 17 2022" -A x64 -DLIBTMUX_BUILD_TESTS=OFF -DLIBTMUX_BUILD_EXAMPLES=OFF
```

```console
$ cmake --build build/libtmux --config Release
```

```console
$ cmake --install build/libtmux --config Release --prefix "$PWD/build/libtmux-prefix"
```

```console
$ cmake -S . -B build/consume -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="$PWD/build/libtmux-prefix"
```

```console
$ cmake --build build/consume --config Release
```

```console
$ .\build\consume\Release\consume.exe
```

This consumer never contacts psmux. The repository's native Windows smoke is
the separate runtime gate documented in the
[Windows compatibility section](../../README.md#windows-through-psmux).

## Why it exists

This is the only thing here that tests the **package** rather than the build
tree — whether the installed headers, the compiled library and the exported
CMake config actually work together from outside. Continuous integration builds
it against a real install on every change, in both the C++23 and C++20
configurations, which is how the package config is kept from rotting.

It deliberately **never contacts tmux**. Whether the library talks to tmux
correctly is what the [test suite](../../tests/README.md) and the other
[examples](../README.md) are for; this one answers a different question, and
answering only that question is what makes a failure here easy to read.

It does exercise a real slice of the surface without a server: version parsing,
a filter over a recorded snapshot, and a chain refusing a target it cannot
address. Each is a compiled call into the installed library, so a package that
exported the wrong thing fails here rather than in someone else's project.

## Related

- [The library](../../README.md#installation) — the other ways to depend on it
- [`examples/`](../README.md) — the programs that do talk to tmux
- [`ports/libtmux/`](../../ports/libtmux/) — the vcpkg port, which does exactly this
