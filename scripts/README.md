# Scripts

The support scripts CI, releases and maintainers run. **None of it is installed
or exported.** [`.github/SCRIPTS.md`](../.github/SCRIPTS.md) says what may stay
here, and each directory holds a script with its README and tests.

The scripts are typed Python. Each declares its dependencies inline
([PEP 723](https://peps.python.org/pep-0723/)) and runs from the repository root
with `uv run`. [`pyproject.toml`](../pyproject.toml) holds the dev dependency
group and the settings that `ruff`, `ty` and `pytest` apply to every script in
CI:

```console
$ uv run --group dev ruff format --check .
```

```console
$ uv run --group dev ruff check .
```

```console
$ uv run --group dev ty check scripts
```

```console
$ uv run --group dev pytest
```

| Directory | What it answers | Run by |
|---|---|---|
| [`ci_plan/`](ci_plan/) | Which tmux releases CI runs, and whether the docs and workflows agree with that list | `ci.yml` |
| [`docs/`](docs/) | Whether the API reference, README code, links, commands and changelogs match the source | `ci.yml`, `release.yml`, CTest |
| [`vcpkg/`](vcpkg/) | Whether the registry's versions database matches the ports, and whether a consumer resolves it | `ci.yml`, `release.yml` |
| [`mutate/`](mutate/) | Whether the tests can fail | `ci.yml` |
| [`coverage/`](coverage/) | How much of the shipped surface the tests reach | `ci.yml` |
| [`include_cost/`](include_cost/) | What including the umbrella header costs | `ci.yml` |
| [`installed_probe/`](installed_probe/) | Whether the installed `tmux-workspace` and MCP server answer as a user needs | `ci.yml`, `release.yml` |
| [`schema/`](schema/) | Whether emitted documents validate against the published JSON Schemas | CTest |
| [`mcp_inventory/`](mcp_inventory/) | Whether the MCP README lists the tools the server advertises | CTest |
| [`workspace_cli/`](workspace_cli/) | Whether the installed workspace CLI handles terminals, processes and output limits | CTest |
| [`psmux/`](psmux/) | Windows: the example and MCP server against a psmux session the run owns | CTest |

[`tools/`](../tools/README.md) holds the one tool written in C++.
