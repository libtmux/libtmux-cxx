# Testing API reference

`libtmux::testing` — the private-tmux-server fixture this project's own
suite runs on, shipped so a consumer's suite can run on it too. Ask for
it with `find_package(libtmux COMPONENTS testing)`; it is not part of
the library, and a program that only uses libtmux links none of it.

Generated from the headers by `tools/docs/api_index.py`; the prose here
is the prose there. Run it with `--check` to prove this page is current.

## `libtmux/testing/scoped_server.hpp`

A tmux server that belongs to one test, and dies with it.  This ships. Anyone writing tests *against* this library has the same problem its own suite has — a tmux server is shared state keyed only by its socket, so two suites that pick the same one delete each other's sessions and produce failures that look like bugs in whichever noticed first. Solving that correctly means a private `$TMUX_TMPDIR`, a socket under it, an erased `TMUX`/`TMUX_PANE` so a suite run from inside tmux cannot reach outward, and a teardown that reaps the server even when the test aborts. That is too much to ask every consumer to rediscover, and a consumer who gets it wrong takes somebody's real session with them.  So it is exported as `libtmux::testing`, separately from the library: a program that only *uses* libtmux links none of this.

### SocketNamespace

```cpp
[[nodiscard]] static SocketNamespace internal();
```
The house default, kept as a named thing so a consumer can say it meant this one rather than spelling the string again.

```cpp
[[nodiscard]] static SocketNamespace consumer(std::string_view suite);
```
For a suite that consumes the installed package — the examples' own tests are the first, and the point of the split: their servers are visibly not this library's.

### Free functions

```cpp
void set_environment(std::vector<std::string>& environment, std::string_view name, std::string_view value);
```
Editing a `NAME=VALUE` block of the kind `child_environment()` returns.  Public because `child_environment()` is: handing a caller a vector of strings and leaving them to work out that a name may appear only once, and that `TMUX=` is not the same as `TMUX` being absent, would be handing them a bug. `set_environment` replaces rather than appends; `erase_environment` removes every entry for the name.

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
The environment a child needs to address this server as tmux would: an exported `TMUX_TMPDIR` pointing into the private tree, and `TMUX` and `TMUX_PANE` erased so the child cannot reach the server the suite itself is running inside. Entries are `NAME=VALUE`; a name with no `=` means "remove this one".  This exists because the examples are programs, not functions: the only way to test one is to run it, and the only way to keep it off a real server is to hand it this.

## `libtmux/testing/capabilities.hpp`

Behaviour that belongs to tmux rather than to this library, as GoogleTest skips.  On a release that cannot provide a capability the honest outcome is a skip that names the release and the reason — not a failure that reads like a bug here, and not a silent deletion that would stop covering the versions that do provide it.  Two shapes, and the difference matters. `LIBTMUX_REQUIRES_TMUX` is for a capability that arrived in some release and has been there since. Some defects are not that shape: tmux 3.4 answered a round-tripped option with an escape that 3.3a and 3.5 both get right. A minimum would wrongly excuse every release above it, so those get a closed window instead.  This header is the only part of `libtmux::testing` that knows what a test framework is, which is why it is a separate target: link `libtmux::testing_gtest` to get it, or use `tmux_version.hpp` directly and write the skip in whatever framework you actually use.

### Free functions

```cpp
GTEST_SKIP() << (capability) << " arrived in tmux " << (major_version) << '.' \ << (minor_version) << ";
```

```cpp
GTEST_SKIP() << (description) << " (tmux " << (first_major) << '.' \ << (first_minor) << " through " << (last_major) << '.' \ << (last_minor) << ";
```

## `libtmux/testing/tmux_version.hpp`

Which tmux the tests are actually running against.  The library supports 3.2a and newer, and across that range tmux does not answer identically: some capabilities arrived later, and a few releases answered a question wrongly and a later one fixed it. A test asserting such a behaviour is asserting something about the tmux underneath, so it has to be able to ask which one that is.  Resolved once, at runtime, from the same binary `ScopedTmuxServer` will spawn — not stamped in by the build. The build-time stamp was a hazard: it recorded whatever `find_program` saw at configure time, which is not necessarily what runs, and it made the fixture impossible to install (a shipped library cannot carry the consumer's tmux version). Asking the binary the fixture is about to launch cannot disagree with the binary the fixture launches.  No test framework is involved. `capabilities.hpp` layers GoogleTest skip macros on top for suites that want them; a suite on Catch2 or doctest uses this directly.

### Free functions

```cpp
[[nodiscard]] const Version& running_tmux(const std::filesystem::path& tmux_binary = "tmux");
```
The version of `tmux_binary`, resolved once per distinct binary and cached.  An unreadable or unrunnable tmux reports as the newest possible version rather than the oldest. A test that then fails says something true about this tmux; a test wrongly skipped says nothing at all, and that is the worse of the two.

```cpp
[[nodiscard]] std::string describe_running_tmux(const std::filesystem::path& tmux_binary = "tmux");
```
What `tmux -V` printed, verbatim, for putting in a skip message.

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
