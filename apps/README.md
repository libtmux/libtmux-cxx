# Applications

Programs built on the library, as opposed to the library itself. Each is its
own package with its own dependencies, and none of them is built by default.

| Application | What it is |
|---|---|
| [`mcp/`](mcp/README.md) | A [Model Context Protocol](https://modelcontextprotocol.io) server, so an agent can drive tmux ([changelog](mcp/CHANGELOG.md)) |
| [`workspace/`](workspace/README.md) | `tmux-workspace`, a command that loads, captures and converts tmuxp-style workspace files ([changelog](workspace/CHANGELOG.md)) |
| [`matrix/`](matrix/README.md) | `libtmux_matrix`, a benchmark that prints what each way of reaching tmux costs |

Each also puts weight on the library. The MCP server takes untyped string
arguments from a model, which exercises validation and error reporting;
[`examples/workspace/`](../examples/workspace/README.md) pushes on composition.

## Related

- [The library](../README.md) — what these are built on
- [`examples/`](../examples/README.md) — smaller programs, read top to bottom
