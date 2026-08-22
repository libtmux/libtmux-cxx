# Testing API reference

`libtmux::testing` — the private-tmux-server fixture this project's own
suite runs on, shipped so a consumer's suite can run on it too. Ask for
it with `find_package(libtmux COMPONENTS testing)`; it is not part of
the library, and a program that only uses libtmux links none of it.

Generated from the headers by `tools/docs/api_index.py`; the prose here
is the prose there. Run it with `--check` to prove this page is current.

## Headers

- [`libtmux/testing/scoped_server.hpp`](#libtmux-testing-scoped-server-hpp)
- [`libtmux/testing/capabilities.hpp`](#libtmux-testing-capabilities-hpp)
- [`libtmux/testing/tmux_version.hpp`](#libtmux-testing-tmux-version-hpp)
- [`libtmux/testing/environment_guard.hpp`](#libtmux-testing-environment-guard-hpp)

<a id="libtmux-testing-scoped-server-hpp"></a>
## `libtmux/testing/scoped_server.hpp`

A private tmux server, torn down with the scope that started it.  Exported as `libtmux::testing`, a target separate from the library. The server gets a `mkdtemp` tree of its own, `TMUX_TMPDIR` inside it, and a child environment with `TMUX` and `TMUX_PANE` erased, so a suite run from inside tmux cannot reach the surrounding server. Teardown kills the server and removes the tree even when a test aborts.

**Symbols:**

- [`SocketMode`](#libtmux-testing-scoped-server-hpp-socketmode)
  - [`SocketMode::Name`](#libtmux-testing-scoped-server-hpp-socketmode-name)
  - [`SocketMode::Path`](#libtmux-testing-scoped-server-hpp-socketmode-path)
- [`TeardownReport`](#libtmux-testing-scoped-server-hpp-teardownreport)
  - [`TeardownReport::messages`](#libtmux-testing-scoped-server-hpp-teardownreport-messages)
- [`SocketNamespace`](#libtmux-testing-scoped-server-hpp-socketnamespace)
  - [`SocketNamespace::label`](#libtmux-testing-scoped-server-hpp-socketnamespace-label)
  - [`SocketNamespace::internal`](#libtmux-testing-scoped-server-hpp-socketnamespace-internal)
  - [`SocketNamespace::consumer`](#libtmux-testing-scoped-server-hpp-socketnamespace-consumer)
- [`ScopedTmuxServerOptions`](#libtmux-testing-scoped-server-hpp-scopedtmuxserveroptions)
  - [`ScopedTmuxServerOptions::tmux_binary`](#libtmux-testing-scoped-server-hpp-scopedtmuxserveroptions-tmux-binary)
  - [`ScopedTmuxServerOptions::mode`](#libtmux-testing-scoped-server-hpp-scopedtmuxserveroptions-mode)
  - [`ScopedTmuxServerOptions::startup_timeout`](#libtmux-testing-scoped-server-hpp-scopedtmuxserveroptions-startup-timeout)
  - [`ScopedTmuxServerOptions::teardown_timeout`](#libtmux-testing-scoped-server-hpp-scopedtmuxserveroptions-teardown-timeout)
  - [`ScopedTmuxServerOptions::session_name`](#libtmux-testing-scoped-server-hpp-scopedtmuxserveroptions-session-name)
  - [`ScopedTmuxServerOptions::socket_namespace`](#libtmux-testing-scoped-server-hpp-scopedtmuxserveroptions-socket-namespace)
  - [`ScopedTmuxServerOptions::teardown_report`](#libtmux-testing-scoped-server-hpp-scopedtmuxserveroptions-teardown-report)
- [`ScopedTmuxServer`](#libtmux-testing-scoped-server-hpp-scopedtmuxserver)
  - [`ScopedTmuxServer::start`](#libtmux-testing-scoped-server-hpp-scopedtmuxserver-start)
  - [`ScopedTmuxServer::~ScopedTmuxServer`](#libtmux-testing-scoped-server-hpp-scopedtmuxserver-scopedtmuxserver)
  - [`ScopedTmuxServer::ScopedTmuxServer`](#libtmux-testing-scoped-server-hpp-scopedtmuxserver-scopedtmuxserver-2)
  - [`ScopedTmuxServer::operator=`](#libtmux-testing-scoped-server-hpp-scopedtmuxserver-operator)
  - [`ScopedTmuxServer::ScopedTmuxServer`](#libtmux-testing-scoped-server-hpp-scopedtmuxserver-scopedtmuxserver-3)
  - [`ScopedTmuxServer::operator=`](#libtmux-testing-scoped-server-hpp-scopedtmuxserver-operator-2)
  - [`ScopedTmuxServer::socket_mode`](#libtmux-testing-scoped-server-hpp-scopedtmuxserver-socket-mode)
  - [`ScopedTmuxServer::socket_name`](#libtmux-testing-scoped-server-hpp-scopedtmuxserver-socket-name)
  - [`ScopedTmuxServer::socket_path`](#libtmux-testing-scoped-server-hpp-scopedtmuxserver-socket-path)
  - [`ScopedTmuxServer::tmux_tmpdir`](#libtmux-testing-scoped-server-hpp-scopedtmuxserver-tmux-tmpdir)
  - [`ScopedTmuxServer::session_name`](#libtmux-testing-scoped-server-hpp-scopedtmuxserver-session-name)
  - [`ScopedTmuxServer::socket_namespace`](#libtmux-testing-scoped-server-hpp-scopedtmuxserver-socket-namespace)
  - [`ScopedTmuxServer::server_pid`](#libtmux-testing-scoped-server-hpp-scopedtmuxserver-server-pid)
  - [`ScopedTmuxServer::command_prefix`](#libtmux-testing-scoped-server-hpp-scopedtmuxserver-command-prefix)
  - [`ScopedTmuxServer::is_alive`](#libtmux-testing-scoped-server-hpp-scopedtmuxserver-is-alive)
  - [`ScopedTmuxServer::child_environment`](#libtmux-testing-scoped-server-hpp-scopedtmuxserver-child-environment)
- [`Free symbols`](#libtmux-testing-scoped-server-hpp-free-symbols)
  - [`current_environment`](#libtmux-testing-scoped-server-hpp-free-symbols-current-environment)
  - [`set_environment`](#libtmux-testing-scoped-server-hpp-free-symbols-set-environment)
  - [`erase_environment`](#libtmux-testing-scoped-server-hpp-free-symbols-erase-environment)

<a id="libtmux-testing-scoped-server-hpp-socketmode"></a>
### `SocketMode`

```cpp
enum class SocketMode;
```

<a id="libtmux-testing-scoped-server-hpp-socketmode-name"></a>
#### `SocketMode::Name` — `Name,`

<a id="libtmux-testing-scoped-server-hpp-socketmode-path"></a>
#### `SocketMode::Path` — `Path,`

<a id="libtmux-testing-scoped-server-hpp-teardownreport"></a>
### `TeardownReport`

```cpp
struct TeardownReport;
```

<a id="libtmux-testing-scoped-server-hpp-teardownreport-messages"></a>
#### `TeardownReport::messages`

```cpp
std::vector<std::string> messages;
```

<a id="libtmux-testing-scoped-server-hpp-socketnamespace"></a>
### `SocketNamespace`

Names the private tree, the socket and the `tmux -L` name, so concurrent suites cannot collide and a leftover directory identifies its owner.  `label` reaches the socket path, which must fit in `sockaddr_un::sun_path` — 108 bytes on Linux, 104 on macOS. Keep it under about twenty characters: macOS spends roughly sixty of those on `$TMPDIR` alone, once `/var/folders/...` has canonicalised to `/private/var/...`, and tmux spends more again on the directory it puts the socket in. A label that overruns is reported by tmux as "File name too long", naming the path.  `start` refuses one containing anything outside `[A-Za-z0-9._-]`.

```cpp
struct SocketNamespace;
```

<a id="libtmux-testing-scoped-server-hpp-socketnamespace-label"></a>
#### `SocketNamespace::label`

```cpp
std::string label{"libtmux-cxx-test"};
```

<a id="libtmux-testing-scoped-server-hpp-socketnamespace-internal"></a>
#### `SocketNamespace::internal`

```cpp
[[nodiscard]] static SocketNamespace internal();
```

<a id="libtmux-testing-scoped-server-hpp-socketnamespace-consumer"></a>
#### `SocketNamespace::consumer`

```cpp
[[nodiscard]] static SocketNamespace consumer(std::string_view suite);
```
Prefixed, so a consumer's servers are distinguishable from this library's.

<a id="libtmux-testing-scoped-server-hpp-scopedtmuxserveroptions"></a>
### `ScopedTmuxServerOptions`

```cpp
struct ScopedTmuxServerOptions;
```

<a id="libtmux-testing-scoped-server-hpp-scopedtmuxserveroptions-tmux-binary"></a>
#### `ScopedTmuxServerOptions::tmux_binary`

```cpp
std::filesystem::path tmux_binary{"tmux"};
```

<a id="libtmux-testing-scoped-server-hpp-scopedtmuxserveroptions-mode"></a>
#### `ScopedTmuxServerOptions::mode`

```cpp
SocketMode mode{SocketMode::Path};
```

<a id="libtmux-testing-scoped-server-hpp-scopedtmuxserveroptions-startup-timeout"></a>
#### `ScopedTmuxServerOptions::startup_timeout`

```cpp
std::chrono::milliseconds startup_timeout{5000};
```

<a id="libtmux-testing-scoped-server-hpp-scopedtmuxserveroptions-teardown-timeout"></a>
#### `ScopedTmuxServerOptions::teardown_timeout`

```cpp
std::chrono::milliseconds teardown_timeout{2000};
```

<a id="libtmux-testing-scoped-server-hpp-scopedtmuxserveroptions-session-name"></a>
#### `ScopedTmuxServerOptions::session_name`

```cpp
std::string session_name{"libtmux_test"};
```

<a id="libtmux-testing-scoped-server-hpp-scopedtmuxserveroptions-socket-namespace"></a>
#### `ScopedTmuxServerOptions::socket_namespace`

```cpp
SocketNamespace socket_namespace{};
```

<a id="libtmux-testing-scoped-server-hpp-scopedtmuxserveroptions-teardown-report"></a>
#### `ScopedTmuxServerOptions::teardown_report`

```cpp
std::shared_ptr<TeardownReport> teardown_report{};
```
Every member has an initializer: without one here, a designated initializer that stops short of it warns under `-Wmissing-field-initializers`.

<a id="libtmux-testing-scoped-server-hpp-scopedtmuxserver"></a>
### `ScopedTmuxServer`

```cpp
class ScopedTmuxServer final;
```

<a id="libtmux-testing-scoped-server-hpp-scopedtmuxserver-start"></a>
#### `ScopedTmuxServer::start`

```cpp
static libtmux::expected<ScopedTmuxServer, std::string> start(ScopedTmuxServerOptions options = {});
```

<a id="libtmux-testing-scoped-server-hpp-scopedtmuxserver-scopedtmuxserver"></a>
#### `ScopedTmuxServer::~ScopedTmuxServer`

```cpp
~ScopedTmuxServer() noexcept;
```

<a id="libtmux-testing-scoped-server-hpp-scopedtmuxserver-scopedtmuxserver-2"></a>
#### `ScopedTmuxServer::ScopedTmuxServer`

```cpp
ScopedTmuxServer(ScopedTmuxServer&&) noexcept;
```

<a id="libtmux-testing-scoped-server-hpp-scopedtmuxserver-operator"></a>
#### `ScopedTmuxServer::operator=`

```cpp
ScopedTmuxServer& operator=(ScopedTmuxServer&&) noexcept;
```

<a id="libtmux-testing-scoped-server-hpp-scopedtmuxserver-scopedtmuxserver-3"></a>
#### `ScopedTmuxServer::ScopedTmuxServer`

```cpp
ScopedTmuxServer(const ScopedTmuxServer&) = delete;
```

<a id="libtmux-testing-scoped-server-hpp-scopedtmuxserver-operator-2"></a>
#### `ScopedTmuxServer::operator=`

```cpp
ScopedTmuxServer& operator=(const ScopedTmuxServer&) = delete;
```

<a id="libtmux-testing-scoped-server-hpp-scopedtmuxserver-socket-mode"></a>
#### `ScopedTmuxServer::socket_mode`

```cpp
[[nodiscard]] SocketMode socket_mode() const noexcept;
```

<a id="libtmux-testing-scoped-server-hpp-scopedtmuxserver-socket-name"></a>
#### `ScopedTmuxServer::socket_name`

```cpp
[[nodiscard]] std::optional<std::string_view> socket_name() const noexcept;
```

<a id="libtmux-testing-scoped-server-hpp-scopedtmuxserver-socket-path"></a>
#### `ScopedTmuxServer::socket_path`

```cpp
[[nodiscard]] const std::filesystem::path& socket_path() const noexcept;
```

<a id="libtmux-testing-scoped-server-hpp-scopedtmuxserver-tmux-tmpdir"></a>
#### `ScopedTmuxServer::tmux_tmpdir`

```cpp
[[nodiscard]] const std::filesystem::path& tmux_tmpdir() const noexcept;
```

<a id="libtmux-testing-scoped-server-hpp-scopedtmuxserver-session-name"></a>
#### `ScopedTmuxServer::session_name`

```cpp
[[nodiscard]] std::string_view session_name() const noexcept;
```

<a id="libtmux-testing-scoped-server-hpp-scopedtmuxserver-socket-namespace"></a>
#### `ScopedTmuxServer::socket_namespace`

```cpp
[[nodiscard]] std::string_view socket_namespace() const noexcept;
```

<a id="libtmux-testing-scoped-server-hpp-scopedtmuxserver-server-pid"></a>
#### `ScopedTmuxServer::server_pid`

```cpp
[[nodiscard]] int server_pid() const noexcept;
```

<a id="libtmux-testing-scoped-server-hpp-scopedtmuxserver-command-prefix"></a>
#### `ScopedTmuxServer::command_prefix`

```cpp
[[nodiscard]] std::vector<std::string> command_prefix() const;
```

<a id="libtmux-testing-scoped-server-hpp-scopedtmuxserver-is-alive"></a>
#### `ScopedTmuxServer::is_alive`

```cpp
[[nodiscard]] bool is_alive() const;
```

<a id="libtmux-testing-scoped-server-hpp-scopedtmuxserver-child-environment"></a>
#### `ScopedTmuxServer::child_environment`

```cpp
[[nodiscard]] std::vector<std::string> child_environment() const;
```
`NAME=VALUE` entries for a child that should reach this server and not the surrounding one: `TMUX_TMPDIR` inside the private tree, `TMUX` and `TMUX_PANE` absent.

<a id="libtmux-testing-scoped-server-hpp-free-symbols"></a>
### `Free symbols`

<a id="libtmux-testing-scoped-server-hpp-free-symbols-current-environment"></a>
#### `current_environment`

```cpp
[[nodiscard]] std::vector<std::string> current_environment();
```
Build and edit a `NAME=VALUE` block for a child process. `current_environment` copies this process's; `set_environment` replaces any existing entry rather than appending; `erase_environment` removes every entry for the name.

<a id="libtmux-testing-scoped-server-hpp-free-symbols-set-environment"></a>
#### `set_environment`

```cpp
void set_environment(std::vector<std::string>& environment, std::string_view name, std::string_view value);
```

<a id="libtmux-testing-scoped-server-hpp-free-symbols-erase-environment"></a>
#### `erase_environment`

```cpp
void erase_environment(std::vector<std::string>& environment, std::string_view name);
```

<a id="libtmux-testing-capabilities-hpp"></a>
## `libtmux/testing/capabilities.hpp`

GoogleTest skips for behaviour that belongs to tmux rather than to this library, so a release that cannot provide a capability is reported as skipped rather than failing or going untested.  `LIBTMUX_REQUIRES_TMUX` is for a capability present from some release onwards. `LIBTMUX_SKIP_TMUX_DEFECT` is for a defect fixed in a later release, where a minimum would wrongly excuse everything above it — tmux 3.4 escapes a round-tripped option that 3.3a and 3.5 both get right.  The only header here that names a test framework. Suites using another one take `tmux_version.hpp` and write their own skip.

**Symbols:**

- [`Free symbols`](#libtmux-testing-capabilities-hpp-free-symbols)
  - [`LIBTMUX_REQUIRES_TMUX`](#libtmux-testing-capabilities-hpp-free-symbols-libtmux-requires-tmux)
  - [`LIBTMUX_SKIP_TMUX_DEFECT`](#libtmux-testing-capabilities-hpp-free-symbols-libtmux-skip-tmux-defect)

<a id="libtmux-testing-capabilities-hpp-free-symbols"></a>
### `Free symbols`

<a id="libtmux-testing-capabilities-hpp-free-symbols-libtmux-requires-tmux"></a>
#### `LIBTMUX_REQUIRES_TMUX`

```cpp
#define LIBTMUX_REQUIRES_TMUX(major_version, minor_version, capability) /* implementation omitted */
```
Skip unless tmux is at least `major.minor`, naming what is missing below it.

<a id="libtmux-testing-capabilities-hpp-free-symbols-libtmux-skip-tmux-defect"></a>
#### `LIBTMUX_SKIP_TMUX_DEFECT`

```cpp
#define LIBTMUX_SKIP_TMUX_DEFECT(first_major, first_minor, last_major, last_minor, description) /* implementation omitted */
```
Skip while tmux is inside a closed range of releases with a known defect. Both ends are inclusive, and `last` is the newest release still affected — so a fix in 3.6 is written as a window ending at 3.5.

<a id="libtmux-testing-tmux-version-hpp"></a>
## `libtmux/testing/tmux_version.hpp`

The tmux the tests run against.  Resolved at runtime from the same binary `ScopedTmuxServer` spawns, not stamped in by the build: an installed library cannot carry the consumer's tmux version, and a configure-time answer can name a different binary than the one that runs.  No test framework is involved. `capabilities.hpp` layers GoogleTest skip macros on top; a suite on Catch2 or doctest uses this directly.

**Symbols:**

- [`Free symbols`](#libtmux-testing-tmux-version-hpp-free-symbols)
  - [`running_tmux`](#libtmux-testing-tmux-version-hpp-free-symbols-running-tmux)
  - [`describe_running_tmux`](#libtmux-testing-tmux-version-hpp-free-symbols-describe-running-tmux)

<a id="libtmux-testing-tmux-version-hpp-free-symbols"></a>
### `Free symbols`

<a id="libtmux-testing-tmux-version-hpp-free-symbols-running-tmux"></a>
#### `running_tmux`

```cpp
[[nodiscard]] Version running_tmux(const std::filesystem::path& tmux_binary = "tmux");
```
Resolved once per distinct binary and cached. A tmux that cannot be run or whose version cannot be parsed reports as the newest possible version, so version-gated tests run and fail rather than silently skipping.

<a id="libtmux-testing-tmux-version-hpp-free-symbols-describe-running-tmux"></a>
#### `describe_running_tmux`

```cpp
[[nodiscard]] std::string describe_running_tmux(const std::filesystem::path& tmux_binary = "tmux");
```
What `tmux -V` printed, verbatim, or "unknown".

<a id="libtmux-testing-environment-guard-hpp"></a>
## `libtmux/testing/environment_guard.hpp`

Set a variable for the length of a scope, and put back exactly what was there.  A socket name is not a location: tmux resolves it under `$TMUX_TMPDIR`, so a test that starts a server under one directory and then asks the library for the same name has to say which directory it means. Exporting it is what a caller of `tmux -L` does, and this is that, scoped.  Process-global, so it is a fixture-setup tool and not something to hold while another thread is spawning: `setenv` racing `getenv` is undefined however careful the caller is.

**Symbols:**

- [`EnvironmentGuard`](#libtmux-testing-environment-guard-hpp-environmentguard)
  - [`EnvironmentGuard::EnvironmentGuard`](#libtmux-testing-environment-guard-hpp-environmentguard-environmentguard)
  - [`EnvironmentGuard::~EnvironmentGuard`](#libtmux-testing-environment-guard-hpp-environmentguard-environmentguard-2)
  - [`EnvironmentGuard::EnvironmentGuard`](#libtmux-testing-environment-guard-hpp-environmentguard-environmentguard-3)
  - [`EnvironmentGuard::operator=`](#libtmux-testing-environment-guard-hpp-environmentguard-operator)
  - [`EnvironmentGuard::EnvironmentGuard`](#libtmux-testing-environment-guard-hpp-environmentguard-environmentguard-4)
  - [`EnvironmentGuard::operator=`](#libtmux-testing-environment-guard-hpp-environmentguard-operator-2)

<a id="libtmux-testing-environment-guard-hpp-environmentguard"></a>
### `EnvironmentGuard`

```cpp
class EnvironmentGuard final;
```

<a id="libtmux-testing-environment-guard-hpp-environmentguard-environmentguard"></a>
#### `EnvironmentGuard::EnvironmentGuard`

```cpp
EnvironmentGuard(std::string name, std::string_view value);
```

<a id="libtmux-testing-environment-guard-hpp-environmentguard-environmentguard-2"></a>
#### `EnvironmentGuard::~EnvironmentGuard`

```cpp
~EnvironmentGuard();
```

<a id="libtmux-testing-environment-guard-hpp-environmentguard-environmentguard-3"></a>
#### `EnvironmentGuard::EnvironmentGuard`

```cpp
EnvironmentGuard(const EnvironmentGuard&) = delete;
```

<a id="libtmux-testing-environment-guard-hpp-environmentguard-operator"></a>
#### `EnvironmentGuard::operator=`

```cpp
EnvironmentGuard& operator=(const EnvironmentGuard&) = delete;
```

<a id="libtmux-testing-environment-guard-hpp-environmentguard-environmentguard-4"></a>
#### `EnvironmentGuard::EnvironmentGuard`

```cpp
EnvironmentGuard(EnvironmentGuard&&) = delete;
```

<a id="libtmux-testing-environment-guard-hpp-environmentguard-operator-2"></a>
#### `EnvironmentGuard::operator=`

```cpp
EnvironmentGuard& operator=(EnvironmentGuard&&) = delete;
```
