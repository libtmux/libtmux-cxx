# Tools

Development tooling. **None of it is installed or exported** — it exists to
check the library, not to ship with it.

The Python portion is a package so that `python3 -m tools.<x>` resolves from
this repository rather than from whatever happens to be on the path, and so the
lint contract in [`pyproject.toml`](../pyproject.toml) travels with the code it
governs. The repository-internal MCP switcher is a native C++ executable built
alongside the tests.

```console
$ python3 -m tools.mutate --preset cxx-dev
```

It is run from the repository root.

## What is here

| Tool | Command | What it answers |
|---|---|---|
| [`mutate/`](mutate/) | `python3 -m tools.mutate` | Whether the tests can actually fail |
| [`mcp/`](mcp/) | `./build/cxx-dev/tools/mcp/mcp-swap` | Points POSIX agent CLI configs at a chosen build of [the MCP server](../apps/mcp/README.md), and puts them back |
| [`scripts/docs/`](../scripts/docs/) | `uv run scripts/docs/api_index.py` | Generates [`docs/api.md`](../docs/api.md) from the headers, so the reference cannot outrun them |
| [`scripts/docs/`](../scripts/docs/) | [README quoting check](../.github/CONTRIBUTING.md#checks-that-must-pass) | Whether the README's C++ still matches the example it is quoted from |
| [`scripts/docs/`](../scripts/docs/) | `uv run scripts/docs/check_example_isolation.py` | Whether each quoted region compiles as the only code in its own file, the way a reader pastes it |
| [`scripts/ci_plan/`](../scripts/ci_plan/) | `uv run scripts/ci_plan/ci_plan.py matrix --event push` | Which tmux versions CI runs for an event, from one manifest |
| [`scripts/ci_plan/`](../scripts/ci_plan/) | `uv run scripts/ci_plan/ci_check.py` | Whether the README, the compatibility page, CONTRIBUTING and the workflows still follow that manifest |
| [`vcpkg/`](vcpkg/) | `python3 -m tools.vcpkg check` | Whether [the registry](../docs/vcpkg-registry.md) still publishes the ports beside it |
| [`coverage/`](coverage/) | — | How much of the shipped surface the tests reach |

## The mutation catalogue

Every finding worth having lately came from this rather than from reading. It
breaks one guard at a time — each one there because tmux does something quiet,
like answering an unresolvable target with a blank and exiting zero — and
reports what nothing noticed.

Three outcomes, and only the first is a pass:

- **killed** — it applied, built, and a test failed. What is wanted.
- **survived** — something is untested, and the report names which guard.
- **not a result** — it did not apply, did not build, or did not reach the
  binary that was then tested. Nothing was learned, and saying so is the point:
  run by hand, that case prints nothing and reads exactly like a suite holding
  firm.

Every entry binds both the executable and its CTest selector. A missing native
binary, including a Windows `.exe`, or a selector matching no tests is `not a
result`. The selected tests must pass before mutation and recover after it, so
an existing failure or a broken restoration cannot be mistaken for a kill.
Platform-specific entries run only with their declared preset; explicitly
requesting one from an incompatible build is an error rather than a skip.

> [!WARNING]
> The runner edits sources in place and puts each back as it goes. Do not
> commit or build from the tree while it runs. A commit taken during a run once
> captured a `load-buffer` without the flag naming its buffer, and the diff read
> like ordinary work.
>
> It rebuilds once on the way out, so a `ctest` straight afterwards runs the
> code that is in the tree rather than the last mutation's binary.

## Related

- [The library](../README.md)
- [`tests/`](../tests/README.md) — what these tools check
- [`docs/`](../docs/) — the design notes and bakeoffs they produce
