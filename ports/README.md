# vcpkg overlay

This is an overlay port, not a submission to the vcpkg registry. Use it with

```console
$ vcpkg install libtmux --overlay-ports=packaging/vcpkg
```

The port builds the library alone. Tests are disabled because they need
GoogleTest and a running tmux, and a consumer installing the library should
acquire neither.

`SHA512` is `0`, which makes vcpkg download the archive and print the hash it
found. Replace it with that value when a release is tagged; until there is a
tag, `--head` is the only mode that resolves.

## Why this is not enough on its own

vcpkg is not installed in this repository's development environment, so this
port is checked by inspection and by the fact that the underlying package
installs and is consumed by `examples/consume` in continuous integration.
Publishing it should wait for a run against real vcpkg on a tagged release.
