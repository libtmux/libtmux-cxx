# CMake modules

Helpers the build uses, and the config template the installed package ships.
Nothing here is part of the public interface except
[`libtmuxConfig.cmake.in`](libtmuxConfig.cmake.in), which becomes the file
`find_package(libtmux)` reads.

| File | Does |
|---|---|
| [`libtmuxConfig.cmake.in`](libtmuxConfig.cmake.in) | The installed package config. What `find_package(libtmux REQUIRED)` finds |
| [`ProjectOptions.cmake`](ProjectOptions.cmake) | Warnings, sanitizers, clang-tidy, and the standard selection |
| [`GoogleTest.cmake`](GoogleTest.cmake) | Finds GoogleTest, or fetches the pinned one when allowed |
| [`NlohmannJson.cmake`](NlohmannJson.cmake) | The same, for the JSON parser the MCP server needs |
| [`Tomlplusplus.cmake`](Tomlplusplus.cmake) | The same, for the private TOML validator used by `mcp-swap` |
| [`YamlCpp.cmake`](YamlCpp.cmake) | The same, for the parser the workspace consumer needs |
| [`toolchains/clang-libcxx.cmake`](toolchains/clang-libcxx.cmake) | The clang-with-libc++ pairing the presets use |

## The resolve-or-fetch pattern

The dependency modules share one shape:

1. `find_package(... QUIET)` — use what the system has
2. If it is missing and `LIBTMUX_FETCH_DEPS` is off, **fail with a message
   naming the flag**
3. Otherwise fetch a pinned version by hash

A build that needs the network says so and stops. The library depends on
nothing; these serve the tests, examples, MCP server and tooling.

## Related

- [The library](../README.md#build-options) — what each option does
- [`ports/libtmux/`](../ports/libtmux/) — the vcpkg port, which drives all of this
- [`CMakePresets.json`](../CMakePresets.json) — the configurations CI builds
