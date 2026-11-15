# Coverage report

Measures how much of the shipped surface the tests reach. CI runs it in the
`coverage` job after `ctest --preset cxx-coverage`. The job fails under a
floor set well below the current figure, so it catches a collapse, such as
instrumentation silently disabled or a suite that stopped running, and not
the drift of a few lines.

It merges the `.profraw` files the run wrote and reports source-based coverage
over `include/libtmux` and `src` with `llvm-cov`, naming every instrumented
binary, because a report over one object omits what the others covered.
Both LLVM tools are the version 18 build.

```console
$ ctest --preset cxx-coverage
```

```console
$ uv run scripts/coverage/coverage_report.py --build build/cxx-coverage --source . --floor 65
```

It refuses to report when the newest profile predates the newest binary, since
that would describe a build that has since changed.

## Tests

```console
$ uv run --group dev pytest scripts/coverage
```

The floor cases use stand-ins for the two LLVM tools, and fail when the
comparison is reversed.
