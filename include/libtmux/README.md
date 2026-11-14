<!-- libtmux-logo -->
<div align="center">
  <picture>
    <source srcset="../../assets/logo.svg" type="image/svg+xml">
    <img src="../../assets/logo.png" width="128" height="128" alt="libtmux for C++">
  </picture>
</div>
<!-- /libtmux-logo -->

<div align="center">

# The public headers

If it is not in this directory, it is not the contract. Everything under
[`src/`](../../src/README.md) is free to change without notice; everything here
is what a consumer may depend on.

[![libtmux.org](https://img.shields.io/badge/docs-libtmux.org-0b7285)](https://libtmux.org/en/cxx/)

</div>

## Start here

```cpp
#include <format>
#include <iostream>

#include <libtmux/libtmux.hpp>
```

- One library include is enough: [`libtmux.hpp`](libtmux.hpp) pulls in the
  rest. The tables below say what each header holds.
- The [generated API reference](../../docs/api.md) has every public type and
  call with the prose from its header. The same pages are on
  [libtmux.org][ref-index].

### [`Server::sessions`][ref-server-sessions]: a first call

[`Server::at_socket_path`][ref-server-at_socket_path] reaches a specific server.
[`Server::sessions`][ref-server-sessions] lists its sessions, and
[`Server::windows`][ref-server-windows] and
[`Server::panes`][ref-server-panes] are its siblings:

```cpp
// Given: const libtmux::Server& server
// No tmux failure is thrown. Every call answers with a value that is either
// the result or the reason there isn't one.
const auto sessions = server.sessions();
if (!sessions.has_value()) {
  std::cerr << std::format("{}\n", sessions.error());
  return 1;
}

for (const libtmux::Session& session : *sessions) {
  std::cout << std::format("{} has {} window(s)\n", session.name(),
                           session.window_count());
}
```

This prints one line per session. If tmux cannot answer, the call returns a
[`CommandFailure`][ref-commandfailure].

## What is where

### Reaching tmux

| Header | Holds |
|---|---|
| [`server.hpp`](server.hpp) | `Server` — the entry point. `from_env`, `at_socket_name`, `at_socket_path`, `at_default`, and `over` for a transport you supply |
| [`capabilities.hpp`](capabilities.hpp) | Local backend and feature discovery, including the bounded psmux surface |
| [`entities.hpp`](entities.hpp) | `Session`, `Window`, `Pane`, `Client`, `Buffer`, and the option structs their creators take |
| [`socket.hpp`](socket.hpp) | How a server is addressed, and what makes an address valid |
| [`target.hpp`](target.hpp) | The id-based targets every operation is addressed by |
| [`delivery.hpp`](delivery.hpp) | `DeliveryStatus` — whether a failed command was not started, written, answered, or left indeterminate |
| [`command.hpp`](command.hpp) | `CommandFailure`, `FailureKind` — the one command error type; `CommandExecutor`, the one method a transport owes |

### Asking questions

| Header | Holds |
|---|---|
| [`filter_expr.hpp`](filter_expr.hpp) | `FilterExpr`, the typed fields, and `tmuxq::matching` |
| [`cardinality.hpp`](cardinality.hpp) | `first`, `exactly_one`, `CardinalityError` |
| [`relations.hpp`](relations.hpp) | Joining one listing to another without a second call per row |
| [`snapshot.hpp`](snapshot.hpp) | The shared listing an entity is one row of, and `kFormatSeparator` |
| [`format.hpp`](format.hpp) | Composing format strings, escaping the `#` that would otherwise expand |
| [`capture.hpp`](capture.hpp) | `CaptureOptions` — what to read out of a pane, and how much |

### Sending work

| Header | Holds |
|---|---|
| [`async.hpp`](async.hpp) | `CommandRuntime` — explicit bounded thread ownership, admission, results, observers, and shutdown |
| [`batch.hpp`](batch.hpp) | One tmux invocation, one fail-fast group |
| [`chain.hpp`](chain.hpp) | Validated as it is built; a bad target never reaches tmux |
| [`control.hpp`](control.hpp) | `Connection` — one held-open POSIX stream of guarded blocks and outside-block events |
| [`notification.hpp`](notification.hpp) | Raw and parsed notifications, plus independent consumer watches |
| [`keys.hpp`](keys.hpp) | Key names, kept apart from the text a pane is sent |
| [`options.hpp`](options.hpp) | Options at the scope that holds them, and what "inherited" means |

### Machinery

| Header | Holds |
|---|---|
| [`expected.hpp`](expected.hpp) | `expected` — `std::expected`, or `tl::expected` under C++20 |
| [`abi.hpp`](abi.hpp) | The inline namespace that keeps the two builds from linking together |
| [`version.hpp`](version.hpp) | `Version`, `parse_version`, `is_supported` — tmux release ordering |
| [`lowering.hpp`](lowering.hpp), [`lowered_node.hpp`](lowered_node.hpp) | Turning a filter into something another process could evaluate |
| [`legacy_lookup.hpp`](legacy_lookup.hpp) | Names the Python library uses, mapped to what they are here |

## Windows through psmux

- **One subprocess per call,** through psmux's `tmux.exe` alias.
- **Unavailable:** persistent control connections and `libtmux::testing`. Unsafe
  creation and mutation fail before dispatch.
- **Trust the server.** A server-side `command-alias` can redefine a built-in
  command.

More: the [Windows preview details][windows-boundary].

## What the headers promise

- **Failure is a value.** Every call returns an [`expected`][ref-expected] of a
  result or a [`CommandFailure`][ref-commandfailure]; no tmux or transport
  failure is signalled by throwing.
- **Narrower than `noexcept`.** The headers allocate, so `std::bad_alloc` can
  come out of them, and a [`CommandObserver`][ref-commandobserver] a caller
  supplies can throw. The promise is about how failure is reported.
- **Concrete transports stay private.** A caller can supply a
  [`CommandExecutor`][ref-commandexecutor] without depending on the subprocess
  implementation. Control connections and command runtimes expose readiness
  descriptors for event-loop integration.
- **Fields are typed.** A flag offers no string operations and a size compares
  as a number, so those mistakes are build errors on a
  [`FilterExpr`][ref-filterexpr] ([`tests/compile/`](../../tests/compile/)).
- **Two ABIs.** The C++23 and C++20 builds live in different inline namespaces,
  so mixing them fails at the link step instead of corrupting memory at run
  time.

## Testing against it

On POSIX, [`testing/`](testing/) is not part of the library. It is
`libtmux::testing`, a separate target carrying the private-tmux-server fixture,
[`ScopedTmuxServer`][ref-test-scopedtmuxserver], that this project's own suite
runs on:

```cmake
find_package(libtmux REQUIRED COMPONENTS testing)
```

`find_package(libtmux)` alone leaves the target undefined, so nothing links the
fixture by accident.

## Packages

- [`libtmux`](#libtmux): this package. Typed, value-semantic servers, sessions,
  windows and panes; it links nothing.
- [`libtmux::testing`](#libtmuxtesting): a private tmux server for your own
  tests, installed as a CMake component.
- [`tmux-workspace`](#tmux-workspace): a command that loads, captures and
  converts tmuxp-style workspace files.
- [`libtmux-mcp-server`](#libtmux-mcp-server): a Model Context Protocol server,
  so an agent can drive tmux.

### [`libtmux`](https://github.com/libtmux/libtmux-cxx/tree/master/include/libtmux)

Typed, value-semantic control of tmux servers, sessions, windows and panes.

Reference: [API][ref-index], [getting started][guide-start],
[concepts][guide-concepts], [changelog](../../CHANGELOG.md).

[`Server::capabilities`][ref-server-capabilities] reports what a
[`Server`][ref-server] handle can promise without starting tmux, so a program
can choose a workflow before its first command.
[`ServerCapabilities`][ref-servercapabilities] answers `supports(...)` for each
[`ServerFeature`][ref-serverfeature]:

```cpp
// Given: const libtmux::Server& server
// Ask what this server promises before choosing a workflow. The answer comes
// from the handle: it starts no process and cannot fail.
const libtmux::ServerCapabilities capabilities = server.capabilities();
const bool pane_io = capabilities.supports(libtmux::ServerFeature::pane_io);
std::cout << std::format("{} over {}: pane input and output supported: {}\n",
                         libtmux::to_string(capabilities.implementation),
                         libtmux::to_string(capabilities.backend), pane_io);
```

### [`libtmux::testing`](https://github.com/libtmux/libtmux-cxx/tree/master/include/libtmux/testing)

A private tmux server for your own tests, removed when the scope ends. POSIX
only.

Reference: [Testing with libtmux][guide-testing],
[`ScopedTmuxServer`][ref-test-scopedtmuxserver],
[headers](testing/), [changelog](../../CHANGELOG.md).

A [`TeardownReport`][ref-test-teardownreport] passed to
[`ScopedTmuxServer::start`][ref-test-scopedtmuxserver-start] collects what
teardown did, including anything it could not clean up:

```cpp
#include <format>
#include <iostream>
#include <memory>
#include <string>

#include <libtmux/libtmux.hpp>
#include <libtmux/testing/scoped_server.hpp>
```

```cpp
// A private server, and the messages its teardown recorded.
{
  const auto report = std::make_shared<libtmux::test::TeardownReport>();
  {
    auto scoped = libtmux::test::ScopedTmuxServer::start({
        .socket_namespace = libtmux::test::SocketNamespace::consumer("report"),
        .teardown_report = report,
    });
    if (!scoped.has_value()) {
      std::cerr << std::format("{}\n", scoped.error());
      return 1;
    }
  }
  for (const std::string& message : report->messages) {
    std::cout << std::format("teardown: {}\n", message);
  }
}
```

### [`tmux-workspace`](https://github.com/libtmux/libtmux-cxx/tree/master/apps/workspace)

Loads, captures, converts and imports tmuxp-style workspace files from the
command line. Optional; POSIX only.

Reference: [command manual][ws-cli], [`load`][ws-load],
[workspace configuration][ws-configuration],
[README](../../apps/workspace/README.md),
[changelog](../../apps/workspace/CHANGELOG.md).

Build and install it:

```console
$ cmake -S . -B build/workspace \
    -DLIBTMUX_BUILD_WORKSPACE_CLI=ON \
    -DLIBTMUX_FETCH_DEPS=ON \
    -DLIBTMUX_BUILD_TESTS=OFF \
    -DLIBTMUX_BUILD_EXAMPLES=OFF \
    && cmake --build build/workspace \
    && cmake --install build/workspace --prefix ~/.local
```

With a workspace file saved as `project.yaml`, load it on a socket named
`project` without attaching:

```console
$ tmux-workspace load \
    -L project \
    -d \
    ./project.yaml
```

### [`libtmux-mcp-server`](https://github.com/libtmux/libtmux-cxx/tree/master/apps/mcp)

A Model Context Protocol server over stdio, so an agent can drive tmux.

Reference: [overview][mcp-index], [tool reference][mcp-tools],
[connecting a client][mcp-guide], [README](../../apps/mcp/README.md),
[changelog](../../apps/mcp/CHANGELOG.md).

Register it with Claude Code, limited to the
[`inspect`][mcp-toolset-inspect] toolset:

```console
$ claude mcp add tmux \
    -e LIBTMUX_TOOLSETS=inspect \
    -- ~/.local/bin/libtmux-mcp-server --socket-name libtmux-agent
```

## Related

- [The library](../../README.md) — installing it, and what using it looks like
- [`docs/api.md`](../../docs/api.md) — the generated reference
- [`docs/api-testing.md`](../../docs/api-testing.md) — the same for
  `libtmux::testing`
- [`src/`](../../src/README.md) — what implements this, and may change freely
- [`tests/`](../../tests/README.md) — where each promise above is pinned

[ref-index]: https://libtmux.org/en/cxx/latest/reference/
[ref-server]: https://libtmux.org/en/cxx/latest/reference/libtmux-server/
[ref-server-capabilities]: https://libtmux.org/en/cxx/latest/reference/libtmux-server-capabilities/
[ref-servercapabilities]: https://libtmux.org/en/cxx/latest/reference/libtmux-servercapabilities/
[ref-serverfeature]: https://libtmux.org/en/cxx/latest/reference/libtmux-serverfeature/
[ref-test-scopedtmuxserver]: https://libtmux.org/en/cxx/latest/reference/libtmux-test-scopedtmuxserver/
[ref-test-scopedtmuxserver-start]: https://libtmux.org/en/cxx/latest/reference/libtmux-test-scopedtmuxserver-start/
[ref-test-teardownreport]: https://libtmux.org/en/cxx/latest/reference/libtmux-test-teardownreport/
[guide-start]: https://libtmux.org/en/cxx/latest/guides/getting-started/
[guide-concepts]: https://libtmux.org/en/cxx/latest/concepts/
[guide-testing]: https://libtmux.org/en/cxx/latest/guides/testing-with-libtmux/
[ws-cli]: https://libtmux.org/en/cxx/latest/workspace/cli/
[ws-load]: https://libtmux.org/en/cxx/latest/workspace/cli/load/
[ws-configuration]: https://libtmux.org/en/cxx/latest/workspace/configuration/
[mcp-index]: https://libtmux.org/en/cxx/latest/mcp/
[mcp-tools]: https://libtmux.org/en/cxx/latest/mcp/tools/
[mcp-guide]: https://libtmux.org/en/cxx/latest/mcp/guides/
[mcp-toolset-inspect]: https://libtmux.org/en/cxx/latest/mcp/reference/libtmux-mcp-toolset-inspect/
[ref-server-sessions]: https://libtmux.org/en/cxx/latest/reference/libtmux-server-sessions/
[ref-server-windows]: https://libtmux.org/en/cxx/latest/reference/libtmux-server-windows/
[ref-server-panes]: https://libtmux.org/en/cxx/latest/reference/libtmux-server-panes/
[ref-server-at_socket_path]: https://libtmux.org/en/cxx/latest/reference/libtmux-server-at_socket_path/
[ref-commandfailure]: https://libtmux.org/en/cxx/latest/reference/libtmux-commandfailure/
[ref-commandobserver]: https://libtmux.org/en/cxx/latest/reference/libtmux-commandobserver/
[ref-commandexecutor]: https://libtmux.org/en/cxx/latest/reference/libtmux-commandexecutor/
[ref-filterexpr]: https://libtmux.org/en/cxx/latest/reference/libtmux-filterexpr/
[ref-expected]: https://libtmux.org/en/cxx/latest/reference/libtmux-expected/
[windows-boundary]: https://github.com/libtmux/libtmux-cxx#windows-through-psmux
