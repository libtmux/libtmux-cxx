# vcpkg registry checks

This repository is a vcpkg git registry: a consumer resolves the port through
`versions/`, not from `ports/` directly. These scripts keep the two honest and
carry a release across.
CI runs `check` and `probe` in the `registry` job, and the tests on Linux and
Windows. `release.yml` runs all three subcommands.

| Command | What it does |
|---|---|
| `check` | Fails when the git-tree recorded for a port's declared version is not the git-tree of `ports/<name>` at `HEAD`. Reads the repository only, so it answers at once |
| `probe` | Writes the two files a consumer writes, points them at this checkout or a public URL, and builds [`examples/consume/`](../../examples/consume/) through the vcpkg toolchain. With `--feature mcp` it also runs the installed server over stdio |
| `release` | Points the port at a release whose tag now exists, from its version and the SHA512 of the archive GitHub generates; `--enable-windows` attests the audited x64 MSVC target |

```console
$ uv run scripts/vcpkg/vcpkg_registry.py check
```

```console
$ uv run scripts/vcpkg/vcpkg_registry.py probe \
    --vcpkg ~/vcpkg \
    --triplet x64-linux \
    --feature mcp
```

[`docs/vcpkg-registry.md`](../../docs/vcpkg-registry.md) explains why the registry
needs both checks and the order a release keeps.

## Tests

```console
$ uv run --group dev pytest scripts/vcpkg
```

The release archive test also fails when `tools/`, `scripts/` or `.github/`
would ship in it.
