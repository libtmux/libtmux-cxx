# Examples

Programs that read top to bottom, plus larger consumers. The POSIX examples
are CTest cases that run against a tmux server of their own. The Windows psmux
example has a [native runner](windows/README.md), and
[`consume/`](consume/README.md) never contacts tmux.

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
[`scratch_server.hpp`](scratch_server.hpp) and kills it on the way out, so your
own tmux is untouched. The header is four lines over `libtmux::testing`;
[`tests/`](tests/README.md) shows using it from outside the package.

These also build as their own project, against an install:

```console
$ cmake -S examples -B build/examples -DCMAKE_PREFIX_PATH="$PWD/build/prefix"
```

## Complete API programs

[`api/`](api/README.md) contains standalone programs for construction, listings,
relations, creation, filtering, input, and capture. Each has its own entry point
and error handling and can be built against an installed package.

## The short ones

| Example | Reads as | Shows |
|---|---|---|
| [`01-tour.cpp`](01-tour.cpp) | Five minutes with the library | Connect, look around, act, read the result. Sessions, windows, panes, buffers, copy mode, formats. |
| [`02-workspace.cpp`](02-workspace.cpp) | Building an arrangement | Windows, splits, layouts, options, environment — without composing a single tmux argument. |
| [`03-filter.cpp`](03-filter.cpp) | Finding things | Typed fields, standard ranges, and cardinality that cannot throw. |
| [`04-errors.cpp`](04-errors.cpp) | What failure looks like | Every way a call can fail and what each one tells you. Nothing throws. |
| [`05-readme.cpp`](05-readme.cpp) | The README, compiled | Every C++ sample in the top-level README lives here. |
| [`06-streaming.cpp`](06-streaming.cpp) | Watching instead of asking | A control connection held open, tmux's events read as they happen, and pane output as it is printed. Then the same stream from a program that owns its event loop, through the descriptor that goes readable when a take would return something. |
| [`07-windows-psmux.cpp`](07-windows-psmux.cpp) | The Windows preview | Connect to an existing session and make exact, read-only entity queries. Its [native runner](windows/README.md) also wraps the MCP server in an isolated fixture. |
| [`08-quickstart.cpp`](08-quickstart.cpp) | The README's quick start | A whole program on a private server: start a session, type a command, wait for its answer. Its output is compared with [`08-quickstart.out`](08-quickstart.out). |

If you read one, read [`04-errors.cpp`](04-errors.cpp) second. The error model
is the part of this library least like the Python one.

### Quoting code in the README

The top-level README quotes [`08-quickstart.cpp`](08-quickstart.cpp) whole and
named regions of [`05-readme.cpp`](05-readme.cpp) verbatim. A region is a span
between two markers carrying the same name, unique in the file:

```cpp
  // #region chain
  // A chain refuses a target it cannot address before reaching tmux at all,
  // so a malformed batch costs nothing.
  libtmux::Chain chain;
  chain.new_window("a:b", "unreachable");
  std::cout << std::format("chain valid: {}\n", chain.valid()); // false
  // #endregion chain
```

[`check_readme.py`](../scripts/docs/check_readme.py) fails when a block matches no
region, and when a region nothing quotes goes stale. Edit the example, run it,
then bring the quote across:

```console
$ uv run scripts/docs/check_readme.py \
    --fix \
    --readme README.md \
    --readme include/libtmux/README.md \
    --readme docs/runtime.md
```

`--fix` matches a block to a region by its first line; a block that matches
none is left alone and reported.

## Consuming the package from outside

[`consume/`](consume/README.md) is a complete project that finds the installed package
and links it — nothing else. It is the shortest answer to "what do I put in my
`CMakeLists.txt`":

```cmake
find_package(libtmux REQUIRED)
target_link_libraries(your_target PRIVATE libtmux::libtmux)
```

## The workspace builder

[`workspace/`](workspace/README.md) is larger, and is not really an example: it is a
consumer that exists to put weight on the public surface and report where that
surface is awkward. It builds a described session, and reads
[tmuxp](https://tmuxp.git-pull.com) documents.

It keeps its YAML parser to itself, so the library links none of it. What
tmuxp's own documents found is in
[`docs/design/workspace-corpus.md`](../docs/design/workspace-corpus.md).

```console
$ ./build/cxx-dev/examples/workspace/tmuxp_corpus_probe ~/src/tmuxp/examples/*.yaml
```

## Related

- [The library](../README.md) — what these are examples of
- [`apps/mcp`](../apps/mcp/README.md) — the other consumer, from the side a
  model sees
- [`tests/`](../tests/README.md) — where behaviour is pinned rather than shown
