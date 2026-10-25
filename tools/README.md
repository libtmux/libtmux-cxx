# Tools

Development tooling. **None of it is installed, exported, or linked by the C++
build** — it exists to check the library, not to ship with it.

The Python portion is a package so that `python3 -m tools.<x>` resolves from
this repository rather than from whatever happens to be on the path, and so the
lint contract in [`pyproject.toml`](../pyproject.toml) travels with the code it
governs. The repository-internal MCP switcher is a native C++ executable built
alongside the tests.

```console
$ python3 -m tools.parity coverage
```

```console
$ python3 -m tools.mutate --preset cxx-dev
```

Both are run from the repository root.

## What is here

| Tool | Command | What it answers |
|---|---|---|
| [`parity/`](parity/) | `python3 -m tools.parity` | What of Python libtmux this port answers, and with what evidence |
| [`mutate/`](mutate/) | `python3 -m tools.mutate` | Whether the tests can actually fail |
| [`docs/`](docs/) | `python3 tools/docs/api_index.py` | Generates [`docs/api.md`](../docs/api.md) from the headers, so the reference cannot outrun them |
| [`docs/`](docs/) | `python3 tools/docs/check_readme.py` | Whether the README's C++ still matches the example it is quoted from |
| [`docs/`](docs/) | `python3 -m tools.docs.check_example_isolation` | Whether each quoted region compiles as the only code in its own file, the way a reader pastes it |
| [`mcp/`](mcp/) | `./build/cxx-dev/tools/mcp/mcp-swap` | Points POSIX agent CLI configs at a chosen build of [the MCP server](../apps/mcp/README.md), and puts them back |
| [`vcpkg/`](vcpkg/) | `python3 -m tools.vcpkg check` | Whether [the registry](../docs/vcpkg-registry.md) still publishes the ports beside it |
| [`schema/`](schema/) | `python3 tools/schema/generate_records.py --check` | Whether the committed MCP entity records match their generation digests |
| [`coverage/`](coverage/) | — | How much of the shipped surface the tests reach |
| [`differential/`](differential/) | — | The same question asked of this library and of Python libtmux, compared |
| [`bakeoff/`](bakeoff/) | — | The transport measurements behind [`docs/bakeoffs/`](../docs/bakeoffs/) |
| [`evidence/`](evidence/) | — | Gate records the parity ledger accepts as proof |

## The parity ledger

[`parity/data/`](parity/data/) records what each Python `libtmux` symbol maps
to in this port and the evidence for it. It sits beside the tool that reads it
because it is input, not a deliverable.

`python3 -m tools.parity coverage` reports how much of which part of the
Python surface is answered, and how much of that is recorded with evidence.

Two halves, and only one can run in this repository:

```console
$ python3 -m tools.parity verify --manifest tools/parity/data/manifest.json --mode structural --allow-pending
```

`verify`, `gaps`, `coverage` and `record-evidence` read the recorded artifacts
and this tree. They work.

`generate` and `drift` observe the Python library at a pinned commit to produce
the ledger in the first place. They need that repository's files *and its
history*, neither of which is here, so the recorded surface is frozen at the
commit it was taken from. [AGENTS.md](../AGENTS.md) says so in more detail.

An evidence record cannot cite a test nobody wrote: every `case_id` is resolved
in the file it names, so a `TEST(Suite, Case)` that does not exist fails the
gate rather than passing as a claim.

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

## MCP entity records

[`schema/generate_records.cpp`](schema/generate_records.cpp) owns the annotated
session, window and pane wire fields. Reflection emits
[`output_records.generated.hpp`](../apps/mcp/src/output_records.generated.hpp):
C++20 records, field visitors and a semantic encoder. `schema.cpp` alone builds
JSON schemas from those visitors. Normal builds and installed packages use the
committed output and require neither `<meta>` nor a reflection compiler.

Regenerate with GCC 16.1, `-std=c++26 -freflection`, and clang-format 18:

```console
$ python3 tools/schema/generate_records.py \
    --compiler g++-16
```

Check actual compiler output without writing:

```console
$ python3 tools/schema/generate_records.py \
    --compiler g++-16 \
    --check
```

The normal CI check needs only Python:

```console
$ python3 tools/schema/generate_records.py --check
```

It checks SHA-256 digests of the generator, generation script, formatting policy
and generated header. It detects edits after generation; it does not execute a
compiler or independently prove the transformation. Commit the updated header
and digest file together after running the compiler check. The generator has
also run on the experimental Clang P2996 fork with `-freflection-latest`;
upstream Clang releases are not a regeneration toolchain.

Record strings borrow their entity snapshot only during synchronous encoding.
The returned `StructuredValue` owns its strings. Complete schema and real-tmux
encoder comparisons live in `apps/mcp/tests/schema_test.cpp`.

## Related

- [The library](../README.md)
- [`tests/`](../tests/README.md) — what these tools check
- [`docs/`](../docs/) — the design notes and bakeoffs they produce
