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

The YAML parser is linked by this consumer and the optional
[`tmux-workspace`](../../apps/workspace/README.md) CLI, not the core library.

## Running the corpus probe

Point it at tmuxp's own examples and it reports what this surface cannot yet
express:

```console
$ ./build/cxx-dev/examples/workspace/tmuxp_corpus_probe ~/src/tmuxp/examples/*.yaml
```

## What the pressure found

What real tmuxp documents found in the typed C++ surface, and what remains
unexpressed, is written down in
[`docs/design/workspace-corpus.md`](../../docs/design/workspace-corpus.md).

## Related

- [`examples/`](../README.md) — the programs that read top to bottom
- [`apps/mcp/`](../../apps/mcp/README.md) — the other consumer, pushing on the
  same surface from the side a model sees
- [tmuxp](https://tmuxp.git-pull.com) — the documents this reads
