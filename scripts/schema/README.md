# Schema conformance

Validates what the C++ side emits against the JSON Schemas a consumer in another
language would read. CTest runs both, after the test that writes the corpus.

| Script | Checks |
|---|---|
| [`schema_filter.py`](schema_filter.py) | Every lowered filter expression the `schema.corpus` test wrote validates against [`schema/filter-expression-v1.schema.json`](../../schema/filter-expression-v1.schema.json) |
| [`schema_tool_outputs.py`](schema_tool_outputs.py) | Every MCP tool answer the `consumer.mcp.schema.real-tmux` test wrote validates against the `outputSchema` printed beside it |

Each also requires a table of malformed documents to be refused, because a
schema that accepts everything passes the first half perfectly.

```console
$ uv run scripts/schema/schema_filter.py \
    --schema schema/filter-expression-v1.schema.json \
    --corpus build/cxx-dev/apps/mcp/filter-expressions.jsonl
```

```console
$ uv run scripts/schema/schema_tool_outputs.py \
    --corpus build/cxx-dev/apps/mcp/tool-answers.jsonl
```

CTest runs them with the Python interpreter CMake finds, which needs
`jsonschema` importable. `uv run` installs it from the script's inline metadata.

## Tests

```console
$ uv run --group dev pytest scripts/schema
```

The tests run each validator over a small corpus, and over a schema that
accepts everything, which must fail.
