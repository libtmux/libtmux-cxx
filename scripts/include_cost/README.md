# Include cost

Measures what including the umbrella header costs a translation unit, as a
ratio against a unit that includes only `<string>`, and fails when the ratio is
over budget. CI runs it in the `lint` job, once with clang and libc++ and once
with g++ and libstdc++.

The ratio, not the byte count, is what the budget is written against: the same
headers preprocess to different sizes under each standard library, and the
ratio moves far less.

```console
$ uv run scripts/include_cost/include_cost.py --check
```

```console
$ uv run scripts/include_cost/include_cost.py --compiler g++ --check
```

`--budget` replaces the default, `--header` names another header and
`--standard` another language standard. Without `--check` the ratio is only
printed.

## Tests

```console
$ uv run --project scripts --locked pytest scripts/include_cost
```

The tests compile nothing. A stand-in compiler preprocesses each unit to a size
the test chooses.
