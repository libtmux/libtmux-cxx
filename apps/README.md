# Applications

Programs built on the library, as opposed to the library itself. Each is its
own package with its own dependencies, and none of them is built by default.

| Application | What it is |
|---|---|
| [`mcp/`](mcp/README.md) | A [Model Context Protocol](https://modelcontextprotocol.io) server, so an agent can drive tmux |

They exist for two reasons. The obvious one is that they are useful. The other
is that a library is best judged by something that has to use it: the MCP
server takes every argument as an untyped string from a model, which puts
weight on validation and error reporting that no unit test thought to apply.

The other consumer lives in
[`examples/workspace/`](../examples/workspace/README.md), and pushes on
composition rather than validation.

## Related

- [The library](../README.md) — what these are built on
- [`examples/`](../examples/README.md) — smaller programs, read top to bottom
