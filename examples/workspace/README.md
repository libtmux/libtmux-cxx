# The workspace builder

Not really an example. This is a **consumer**, built to put weight on the public
surface from the outside and report where that surface is awkward — the C++
answer to what [tmuxp](https://tmuxp.git-pull.com) does in Python.

It builds a described session, and reads tmuxp's own YAML documents.

```console
$ cmake --build --preset cxx-dev --target workspace_builder_test
```

```console
$ ctest --preset cxx-dev -R consumer.workspace --output-on-failure
```

## What is here

| Path | What |
|---|---|
| [`include/libtmux_consumers/workspace.hpp`](include/libtmux_consumers/workspace.hpp) | The described-workspace types, and building one against a server |
| [`include/libtmux_consumers/tmuxp.hpp`](include/libtmux_consumers/tmuxp.hpp) | Reading a tmuxp document into that description |
| [`src/tmuxp.cpp`](src/tmuxp.cpp) | The reader, and the only file that knows what YAML is |
| [`tests/`](tests/) | Against real tmux, run as `consumer.workspace` |
| [`tools/corpus_probe.cpp`](tools/corpus_probe.cpp) | Runs a directory of tmuxp documents through the builder and reports what it could not express |

The YAML parser is linked **here and nowhere else**. The library depends on
nothing, and a consumer's parser choice is not the library's business — which
is the point of keeping this in its own directory with its own dependency.

## Running the corpus probe

Point it at tmuxp's own examples and it reports what this surface cannot yet
express:

```console
$ ./build/cxx-dev/examples/workspace/tmuxp_corpus_probe ~/src/tmuxp/examples/*.yaml
```

## What the pressure found

This is the part that justifies the directory. Running real tmuxp documents
through a typed C++ surface found things no unit test was going to:

- `environment:` and `window_index:` were missing from the C++ surface entirely
- every creation call carrying a shell command came back unreadable — a bug in
  the library, found by a consumer rather than by the suite

Both are fixed. What remains unexpressed, and why, is written down in
[`docs/design/workspace-corpus.md`](../../docs/design/workspace-corpus.md).

## Related

- [`examples/`](../README.md) — the programs that read top to bottom
- [`apps/mcp/`](../../apps/mcp/README.md) — the other consumer, pushing on the
  same surface from the side a model sees
- [tmuxp](https://tmuxp.git-pull.com) — the documents this reads
