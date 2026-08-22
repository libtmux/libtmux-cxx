# The vcpkg registry

This repository is a vcpkg [git registry], not just a library that happens to
ship a port. A consumer names this repository's URL and a commit of it, and
vcpkg resolves `libtmux` the same way it resolves anything from the curated
registry — versions, features, and binary caching included.

It serves the port itself because the curated registry is not open to it yet.
vcpkg requires a packaged project to be [mature]: a release six months old, six
months of public development, or official membership of something that already
qualifies. This project is none of those, and
[microsoft/vcpkg#53458](https://github.com/microsoft/vcpkg/pull/53458) was
closed for that reason rather than a technical one. Nothing here is a
workaround — a registry is the answer vcpkg's own documentation gives.

## The order a consumer has to keep

Naming any registry makes the *default* registry's baseline mandatory, and
vcpkg enforces that while loading the configuration — before it runs whatever
you asked it to do. So `vcpkg x-update-baseline --add-initial-baseline`, the
command that would supply that baseline, fails once `vcpkg-configuration.json`
exists:

```
Using registries requires that a baseline is set for the default registry
or that the default registry is null.
```

The manifest therefore comes first and the registry file second, which is the
order [the README](../README.md#vcpkg) gives. A `baseline` of `""` is the
placeholder that survives parsing; the field cannot simply be omitted, because
a git registry is required to have one.

## Layout

vcpkg recognises a registry by two directories. Everything else in the
repository is invisible to it.

| Path | What it is |
|---|---|
| [`ports/libtmux/`](../ports/libtmux/) | The port: manifest, portfile, and the usage text a consumer is shown |
| [`versions/baseline.json`](../versions/baseline.json) | The version each port resolves to by default |
| [`versions/l-/libtmux.json`](../versions/l-/libtmux.json) | Every published version, and the git-tree each was published from |

`ports/` holds nothing but port directories. `x-add-version --all` reads every
entry there as a port and reports an error for anything else, which is why this
file is here rather than in `ports/README.md`.

## The port and the root manifest disagree about Windows on purpose

[`ports/libtmux/vcpkg.json`](../ports/libtmux/vcpkg.json) excludes Windows;
the repository-root [`vcpkg.json`](../vcpkg.json) does not. They are answering
different questions.

The port describes a *published release*, fetched by hash from a tag. The
released archive predates the Windows backend, so the port stays
Windows-disabled until a release exists that carries it — advancing the version
and the `SHA512` together. The root manifest describes the *current checkout*,
which does build natively on Windows.

So the exclusion lifts on the first release cut after the backend lands, not
when the backend lands.

## The versions database is generated

`versions/` is never edited by hand. Commit the port first — the recorded
git-tree describes what is committed, not the working tree — then:

```console
$ vcpkg \
    --x-builtin-ports-root=./ports \
    --x-builtin-registry-versions-dir=./versions \
    x-add-version --all --verbose
```

Changing a published port without changing its version is refused: a consumer
who already resolved that version would otherwise get different code under the
same name. Bump `version-semver` for an upstream release, or `port-version`
when only the packaging changed.

## What a release has to do in order

The portfile fetches a release tarball by hash, and **that hash cannot be known
before the tag exists**. `vcpkg_from_github` downloads the archive GitHub
generates for a tag, which is not byte-identical to one `git archive` produces
locally — same contents, different gzip. So the order is fixed:

1. `VERSION` is bumped and committed.
2. The tag is pushed. [`release.yml`](../.github/workflows/release.yml) refuses
   a tag that disagrees with `VERSION`.
3. GitHub generates the tarball; only now does its hash exist.
4. The port's `version-semver` and `SHA512` are updated, and `x-add-version`
   records the new git-tree.

Step 4 therefore lands on a commit *after* the tag it describes. That is
expected, and it is why a consumer's baseline is not the release tag's commit:
it is the registry commit that follows it. The release notes name the one to
use.

## Checking it

The invariant that matters is that the git-tree recorded for a port's declared
version is the git-tree of that port at `HEAD`. When it is not, the registry
serves the old port while the repository shows the new one.

```console
$ python3 -m tools.vcpkg check
```

This check exists because `x-add-version` does not fail loudly enough to gate
on. Given a port edited without a version bump it prints the disagreement, then
**exits 0 and writes nothing** — so a job keyed on exit status passes, and so
does one keyed on `git diff versions/`, because there is no diff to find.

Agreement between two files is not the same as a consumer being able to resolve
them, so the second check is a real one. It writes the two files a consumer
writes, points them at this checkout, and builds
[`examples/consume/`](../examples/consume/) — which needs no tmux server —
through the vcpkg toolchain:

```console
$ python3 -m tools.vcpkg probe \
    --vcpkg ~/vcpkg \
    --triplet x64-linux \
    --feature mcp
```

That is the only check that exercises resolution: the baseline commit, the
git-tree behind it, and the package config arriving somewhere `find_package`
looks. With `--feature mcp` it also runs the installed server over stdio and
requires it to answer with its tools.

## Related

- [Installing from this registry](../README.md#vcpkg) — the consumer's half
- [`ports/libtmux/`](../ports/libtmux/) — the port
- [Publishing to a git registry][git registry] — vcpkg's own tutorial

[git registry]: https://learn.microsoft.com/vcpkg/produce/publish-to-a-git-registry
[mature]: https://learn.microsoft.com/vcpkg/contributing/maintainer-guide#packaged-projects-should-be-mature
