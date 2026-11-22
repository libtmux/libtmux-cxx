# Mutation catalogue

Checks that the tests can fail. CI runs it in the `mutation` job.

It breaks one guard at a time and reports what nothing noticed. Each guard
exists because tmux does something quiet, such as answering an unresolvable
target with a blank and exiting zero.

Three outcomes, and only the first is a pass:

- **killed**: it applied, built, and a test failed.
- **survived**: something is untested, and the report names which guard.
- **not a result**: it did not apply, did not build, or did not reach the
  binary that was then tested. Nothing was learned, and run by hand that case
  prints nothing and reads exactly like a suite holding firm.

Every entry binds both the executable and its CTest selector. A missing native
binary, including a Windows `.exe`, or a selector matching no tests is `not a
result`. The selected tests must pass before mutation and recover after it, so
an existing failure or a broken restoration cannot be mistaken for a kill.
Platform-specific entries run only with their declared preset; explicitly
requesting one from an incompatible build is an error rather than a skip.

> [!WARNING]
> The runner edits sources in place and puts each back as it goes. Do not
> commit or build from the tree while it runs: a commit taken mid-run captures
> a mutated file.
>
> It rebuilds once on the way out, so a `ctest` straight afterwards runs the
> code that is in the tree rather than the last mutation's binary.

## Run it

From the repository root, configure the preset first. Run it in the project
environment, so a guard that lives in a script can run its pytest test:

```console
$ uv run --project scripts --locked python scripts/mutate/mutate.py \
    --preset cxx-dev
```

`--id` runs one entry and `--build-root` builds somewhere other than the
repository.

A guard may live in Python rather than C++. Such an entry names the file to
break and a pytest node id, and is judged by that test alone, with nothing to
build.

## Tests

```console
$ uv run --project scripts --locked pytest scripts/mutate
```
