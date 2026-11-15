# Documentation checks

Generates the API reference and fails when the READMEs, the examples they quote
or the changelogs disagree with the code. CI runs them in the `lint` job; CTest
runs the ones that need a built program.

| Script | What it answers |
|---|---|
| [`api_index.py`](api_index.py) | Renders `docs/api.md` and `docs/api-testing.md` from the public headers; `--check` fails when a page or the parser fixture has drifted |
| [`check_readme.py`](check_readme.py) | Whether each `cpp` block in a README still matches the example region it quotes; `--fix` brings the quote across |
| [`check_example_isolation.py`](check_example_isolation.py) | Whether each quoted region compiles as the only code in its own file |
| [`check_readme_links.py`](check_readme_links.py) | Whether README links resolve in the checkout, and an API is linked where it is first named; `--online` also fetches the pages |
| [`check_readme_commands.py`](check_readme_commands.py) | Whether the `cmake`, `tmux-workspace` and `libtmux-mcp-server` commands in a README name options, presets and flags that exist |
| [`check_api_examples.py`](check_api_examples.py) | Whether each program in `examples/api/` runs, reports its operation errors and leaves no server behind |
| [`check_changelogs.py`](check_changelogs.py) | Whether the library and each program keep, and install, a changelog that lists the same releases |

Run any of them from the repository root, for example:

```console
$ uv run scripts/docs/api_index.py \
    --include include/libtmux \
    --output docs/api.md \
    --check
```

The commands that start a program take its path. CTest passes the build's
binaries; by hand, name them:

```console
$ uv run scripts/docs/check_readme_commands.py \
    --workspace build/cxx-dev/apps/workspace/tmux-workspace \
    --mcp build/cxx-dev/apps/mcp/libtmux-mcp-server
```

[`fixtures/`](fixtures/) holds the parser's golden input and output, and the
README that [`check_readme_commands.py`](check_readme_commands.py) must refuse.
After a change to the parser, rewrite the golden file once you have read the
diff:

```console
$ uv run scripts/docs/api_index.py --write-fixture
```

## Tests

```console
$ uv run --group dev pytest scripts/docs
```

Each check has cases that break a copy of the input and expect a failure.
