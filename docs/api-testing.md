# Testing API reference

`libtmux::testing` — the private-tmux-server fixture this project's own
suite runs on, shipped so a consumer's suite can run on it too. Ask for
it with `find_package(libtmux COMPONENTS testing)`; it is not part of
the library, and a program that only uses libtmux links none of it.

Generated from the headers by `tools/docs/api_index.py`; the prose here
is the prose there. Run it with `--check` to prove this page is current.

## `libtmux/testing/scoped_server.hpp`

A private tmux server, torn down with the scope that started it.  Exported as `libtmux::testing`, a target separate from the library. The server gets a `mkdtemp` tree of its own, `TMUX_TMPDIR` inside it, and a child environment with `TMUX` and `TMUX_PANE` erased, so a suite run from inside tmux cannot reach the surrounding server. Teardown kills the server and removes the tree even when a test aborts.

### SocketNamespace

```cpp
[[nodiscard]] static SocketNamespace internal();
```

```cpp
[[nodiscard]] static SocketNamespace consumer(std::string_view suite);
```
Prefixed, so a consumer's servers are distinguishable from this library's.

### Free functions

```cpp
void set_environment(std::vector<std::string>& environment, std::string_view name, std::string_view value);
```
Edit a `NAME=VALUE` block of the kind `child_environment()` returns. `set_environment` replaces any existing entry rather than appending; `erase_environment` removes every entry for the name.

```cpp
void erase_environment(std::vector<std::string>& environment, std::string_view name);
```

### ScopedTmuxServer

```cpp
static libtmux::expected<ScopedTmuxServer, std::string> start(ScopedTmuxServerOptions options = {});
```

```cpp
~ScopedTmuxServer() noexcept;
```

```cpp
ScopedTmuxServer(ScopedTmuxServer&&) noexcept;
```

```cpp
ScopedTmuxServer(const ScopedTmuxServer&) = delete;
```

```cpp
[[nodiscard]] SocketMode socket_mode() const noexcept;
```

```cpp
[[nodiscard]] std::optional<std::string_view> socket_name() const noexcept;
```

```cpp
[[nodiscard]] const std::filesystem::path& socket_path() const noexcept;
```

```cpp
[[nodiscard]] const std::filesystem::path& tmux_tmpdir() const noexcept;
```

```cpp
[[nodiscard]] std::string_view session_name() const noexcept;
```

```cpp
[[nodiscard]] std::string_view socket_namespace() const noexcept;
```

```cpp
[[nodiscard]] int server_pid() const noexcept;
```

```cpp
[[nodiscard]] std::vector<std::string> command_prefix() const;
```

```cpp
[[nodiscard]] bool is_alive() const;
```

```cpp
[[nodiscard]] std::vector<std::string> child_environment() const;
```
`NAME=VALUE` entries for a child that should reach this server and not the surrounding one: `TMUX_TMPDIR` inside the private tree, `TMUX` and `TMUX_PANE` absent.

## `libtmux/testing/capabilities.hpp`

GoogleTest skips for behaviour that belongs to tmux rather than to this library, so a release that cannot provide a capability is reported as skipped rather than failing or going untested.  `LIBTMUX_REQUIRES_TMUX` is for a capability present from some release onwards. `LIBTMUX_SKIP_TMUX_DEFECT` is for a defect fixed in a later release, where a minimum would wrongly excuse everything above it — tmux 3.4 escapes a round-tripped option that 3.3a and 3.5 both get right.  The only header here that names a test framework. Suites using another one take `tmux_version.hpp` and write their own skip.

### Free functions

```cpp
GTEST_SKIP() << (capability) << " arrived in tmux " << (major_version) << '.' \ << (minor_version) << ";
```

```cpp
GTEST_SKIP() << (description) << " (tmux " << (first_major) << '.' \ << (first_minor) << " through " << (last_major) << '.' \ << (last_minor) << ";
```

## `libtmux/testing/tmux_version.hpp`

The tmux the tests run against.  Resolved at runtime from the same binary `ScopedTmuxServer` spawns, not stamped in by the build: an installed library cannot carry the consumer's tmux version, and a configure-time answer can name a different binary than the one that runs.  No test framework is involved. `capabilities.hpp` layers GoogleTest skip macros on top; a suite on Catch2 or doctest uses this directly.

### Free functions

```cpp
[[nodiscard]] Version running_tmux(const std::filesystem::path& tmux_binary = "tmux");
```
Resolved once per distinct binary and cached. A tmux that cannot be run or whose version cannot be parsed reports as the newest possible version, so version-gated tests run and fail rather than silently skipping.

```cpp
[[nodiscard]] std::string describe_running_tmux(const std::filesystem::path& tmux_binary = "tmux");
```
What `tmux -V` printed, verbatim, or "unknown".

## `libtmux/testing/environment_guard.hpp`

Set a variable for the length of a scope, and put back exactly what was there.  A socket name is not a location: tmux resolves it under `$TMUX_TMPDIR`, so a test that starts a server under one directory and then asks the library for the same name has to say which directory it means. Exporting it is what a caller of `tmux -L` does, and this is that, scoped.  Process-global, so it is a fixture-setup tool and not something to hold while another thread is spawning: `setenv` racing `getenv` is undefined however careful the caller is.

### EnvironmentGuard

```cpp
EnvironmentGuard(std::string name, std::string_view value) : name_;
```

```cpp
~EnvironmentGuard();
```

```cpp
EnvironmentGuard(const EnvironmentGuard&) = delete;
```

```cpp
EnvironmentGuard(EnvironmentGuard&&) = delete;
```
