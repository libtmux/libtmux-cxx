# Examples

Programs that read top to bottom, plus two larger consumers. The five numbered
ones each run against a real tmux server of their own and are executed as tests
on every build — so nothing here can quietly stop compiling or stop being true.
[`consume/`](consume/README.md) is the exception, and deliberately never
contacts tmux; CI builds and runs it against a real install.

```console
$ cmake --build --preset cxx-dev --target libtmux_example_01_tour
```

```console
$ ./build/cxx-dev/examples/libtmux_example_01_tour
```

Or run them all the way CI does:

```console
$ ctest --preset cxx-dev -R example
```

Each starts a private server through
[`scratch_server.hpp`](scratch_server.hpp) and kills it on the way out, so
running one never touches a tmux you are using. That header is four lines over
`libtmux::testing`, the same fixture the library's own suite runs on — if you
plan to write tests of your own, that is the thing to read, and
[`tests/`](tests/README.md) here is a worked example of using it from outside
the package.

These also build as their own project, against an install rather than this
build tree:

```console
$ cmake -S examples -B build/examples -DCMAKE_PREFIX_PATH="$PWD/build/prefix"
```

That is the only way to prove an example compiles against what a reader would
actually install, rather than against a build tree with this repository's
include paths and warning flags leaking into it.

## The short ones

| Example | Reads as | Shows |
|---|---|---|
| [`01-tour.cpp`](01-tour.cpp) | Five minutes with the library | Connect, look around, act, read the result. Sessions, windows, panes, buffers, copy mode, formats. |
| [`02-workspace.cpp`](02-workspace.cpp) | Building an arrangement | Windows, splits, layouts, options, environment — without composing a single tmux argument. |
| [`03-filter.cpp`](03-filter.cpp) | Finding things | Typed fields, standard ranges, and cardinality that cannot throw. |
| [`04-errors.cpp`](04-errors.cpp) | What failure looks like | Every way a call can fail and what each one tells you. Nothing throws. |
| [`05-readme.cpp`](05-readme.cpp) | The README, compiled | Every C++ sample in the top-level README lives here, so none of them can stop working. |
| [`06-streaming.cpp`](06-streaming.cpp) | Watching instead of asking | A control connection held open, tmux's events read as they happen, and pane output as it is printed. Nothing polls. |

If you read one, read [`04-errors.cpp`](04-errors.cpp) second. The error model
is the part of this library least like the Python one, and it is easier to see
in a program than in prose.

## Consuming the package from outside

[`consume/`](consume/README.md) is a complete project that finds the installed package
and links it — nothing else. It is the shortest answer to "what do I put in my
`CMakeLists.txt`":

```cmake
find_package(libtmux REQUIRED)
target_link_libraries(your_target PRIVATE libtmux::libtmux)
```

It is built against a real install in CI, so the package config it relies on
cannot rot.

## The workspace builder

[`workspace/`](workspace/README.md) is larger, and is not really an example: it is a
consumer that exists to put weight on the public surface and report where that
surface is awkward. It builds a described session, and reads
[tmuxp](https://tmuxp.git-pull.com) documents.

It keeps its YAML parser to itself, so the library links none of it.

That pressure has paid: running tmuxp's own example documents through it
found `environment:` and `window_index:` missing from the C++ surface, and a
bug where every creation call carrying a shell command came back unreadable.
The findings and what remains unread are in
[`docs/design/workspace-corpus.md`](../docs/design/workspace-corpus.md).

```console
$ ./build/cxx-dev/examples/workspace/tmuxp_corpus_probe ~/src/tmuxp/examples/*.yaml
```

## Related

- [The library](../README.md) — what these are examples of
- [`apps/mcp`](../apps/mcp/README.md) — the other consumer, from the side a
  model sees
- [`tests/`](../tests/README.md) — where behaviour is pinned rather than shown
