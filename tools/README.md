# Tools

Development tooling. **None of it is installed or exported** — it exists to
check the library, not to ship with it.

The Python portion is a package so that `python3 -m tools.<x>` resolves from
this repository rather than from whatever happens to be on the path, and so the
lint contract in [`pyproject.toml`](../pyproject.toml) travels with the code it
governs. The repository-internal MCP switcher is a native C++ executable built
alongside the tests.

## What is here

| Tool | Command | What it answers |
|---|---|---|
| [`scripts/mutate/`](../scripts/mutate/) | `uv run --group dev python scripts/mutate/mutate.py` | Whether the tests can actually fail |
| [`mcp/`](mcp/) | `./build/cxx-dev/tools/mcp/mcp-swap` | Points POSIX agent CLI configs at a chosen build of [the MCP server](../apps/mcp/README.md), and puts them back |
| [`scripts/docs/`](../scripts/docs/) | `uv run scripts/docs/api_index.py` | Generates [`docs/api.md`](../docs/api.md) from the headers, so the reference cannot outrun them |
| [`scripts/docs/`](../scripts/docs/) | [README quoting check](../.github/CONTRIBUTING.md#checks-that-must-pass) | Whether the README's C++ still matches the example it is quoted from |
| [`scripts/docs/`](../scripts/docs/) | `uv run scripts/docs/check_example_isolation.py` | Whether each quoted region compiles as the only code in its own file, the way a reader pastes it |
| [`scripts/ci_plan/`](../scripts/ci_plan/) | `uv run scripts/ci_plan/ci_plan.py matrix --event push` | Which tmux versions CI runs for an event, from one manifest |
| [`scripts/ci_plan/`](../scripts/ci_plan/) | `uv run scripts/ci_plan/ci_check.py` | Whether the README, the compatibility page, CONTRIBUTING and the workflows still follow that manifest |
| [`vcpkg/`](vcpkg/) | `python3 -m tools.vcpkg check` | Whether [the registry](../docs/vcpkg-registry.md) still publishes the ports beside it |
| [`scripts/coverage/`](../scripts/coverage/) | `uv run scripts/coverage/coverage_report.py` | How much of the shipped surface the tests reach |
| [`scripts/include_cost/`](../scripts/include_cost/) | `uv run scripts/include_cost/include_cost.py --check` | What including the umbrella header costs a translation unit |

## Related

- [The library](../README.md)
- [`tests/`](../tests/README.md) — what these tools check
- [`docs/`](../docs/) — the design notes and bakeoffs they produce
