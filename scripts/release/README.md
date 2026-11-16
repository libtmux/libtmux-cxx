# Release steps

The steps of `release.yml` whose logic does not fit a one-command `run:` line.
Each subcommand reads the tag and commit from `GITHUB_REF_NAME` and
`GITHUB_SHA`, and appends the values a later step needs to `GITHUB_OUTPUT`. The
workflow calls one per step, and a failure prints one line on stderr and exits
1.

| Command | What it does |
|---|---|
| `tag-fixed` | Fails when the origin's tag no longer names the event commit. `--local-tag` also compares the checked-out tag, `--ancestor-of REF` requires the commit to be merged into `REF`, and `--fetch-master` with `--master-baseline SHA` requires `origin/master` to be at `SHA` |
| `tag-version` | Fails when the tag, without its `v`, differs from `VERSION` |
| `source-archive` | Builds `libtmux-cxx-VERSION.tar.gz` from the event commit and outputs its version and SHA512 |
| `port-hash` | Downloads the archive GitHub generates for the tag, retrying while it lags, requires the same files, kinds, modes and link targets as the event commit, and outputs its SHA512, which the vcpkg port needs |
| `seal-port` | Requires that only the port's portfile and manifest changed, writes the change as a patch, and outputs the patch's SHA256 and the port tree before and after |
| `apply-validated-port` | Checks the patch against its SHA256 and applies it to the port tree it was made from; outputs `already=true` when the tree already is the validated one |
| `commit-versions` | Commits the port transition, runs `vcpkg x-add-version --all` and amends the versions database into the commit |
| `record-baseline` | Outputs `HEAD` as the registry baseline |
| `head-is` | Fails unless `HEAD` is the commit named by `--commit` |
| `push-baseline` | Pushes `HEAD` to `master` with the workflow token, sent as a masked HTTP header |
| `name-asset` | Copies a file to the name a release asset takes, with `{version}` replaced by the tag without its `v`, and writes its SHA256 beside it, in `sha256sum` format; it runs on Linux, macOS and Windows |

```console
$ GITHUB_REF_NAME=v0.1.0 GITHUB_SHA=HEAD \
    uv run scripts/release/release_steps.py tag-version
```

`release.yml` only runs on a pushed tag, so nothing here is run by a pull
request except its tests, which build temporary Git repositories with a bare
origin:

```console
$ uv run --group dev pytest scripts/release
```

`push-baseline` talks to GitHub and has no test. The Windows half of the port
check is [`../vcpkg/windows_port_gate.py`](../vcpkg/windows_port_gate.py).
