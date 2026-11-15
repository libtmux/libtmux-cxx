# Tools

Development tooling written in C++. **None of it is installed or exported** — it
exists to serve the repository, not to ship with it.

## What is here

| Tool | Command | What it answers |
|---|---|---|
| [`mcp/`](mcp/) | `./build/cxx-dev/tools/mcp/mcp-swap` | Points POSIX agent CLI configs at a chosen build of [the MCP server](../apps/mcp/README.md), and puts them back |

The checks, generators and runners CI and maintainers use are Python, and live
in [`scripts/`](../scripts/README.md).

## Related

- [The library](../README.md)
- [`scripts/`](../scripts/README.md) — the support scripts
- [`tests/`](../tests/README.md) — what they check
