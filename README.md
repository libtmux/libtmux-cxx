# libtmux

A typed C++ interface to [tmux](https://github.com/tmux/tmux).

```cpp
#include <libtmux/libtmux.hpp>

// The server this program is running inside, or `at_default()` for the one a
// person means by "my tmux". Every call reports failure by value.
auto server = libtmux::Server::from_env();
auto panes = server->panes();

auto editing = *panes | libtmux::matching(
    libtmux::pane::command.starts_with("nv") && libtmux::pane::active);

if (auto pane = libtmux::first(editing)) {
  (void)pane->get().send_key("Escape");
}
```

Build a workspace, without composing a single tmux argument:

```cpp
auto session = server->new_session({.name = "work", .start_directory = "/srv"});
auto editor = session->new_window({.name = "editor"});
auto logs = editor->split({.horizontal = true, .percentage = 30});
(void)logs->send_text("journalctl -f");
(void)logs->send_key("Enter");
```

## Installing

Requires a C++23 toolchain and CMake 3.25.

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev
```

```console
$ cmake --install build/cxx-dev --prefix /usr/local
```

Consume it with nothing but `find_package`:

```cmake
find_package(libtmux REQUIRED)
target_link_libraries(your_target PRIVATE libtmux::libtmux)
```

`cxx/examples/consume` is a complete working example. A vcpkg overlay port
lives in `packaging/vcpkg`.

## What the design commits to

**An entity is one row of a shared snapshot.** A `Pane`, `Session`, `Window`,
or `Client` holds the listing it came from and the index of its row, so it
copies for a reference count, survives being returned from a function, and
reads its fields without reaching tmux — the process ran once, when the
snapshot was taken. It also reads the moment it was listed rather than the
present: `refresh` returns a new value instead of changing the old one.

**Entities are values.** They compare, hash and print, so a pane keys a map and
a window prints as `Window(@1 2:editor)`. Equality is identity — the same tmux
object on the same connection — so a window refreshed after a rename still
equals the one it was refreshed from.

**Traversal and mutation go through the entity.** `session.windows()`,
`window.split()`, `pane.send_text(...)` — each addressed by the id tmux gave
the object, so no operation can be redirected by a name containing a target
separator. A command that creates something returns it, because tmux prints it;
a command that changes something returns nothing, so nothing is re-read that
the caller did not ask for.

**Options belong to the object that holds them.** `session.set_option(...)`,
`window.option(...)`, `pane.options()`, and the server's own two scopes. A
value that comes from a wider scope is reported as inherited rather than as one
set here, because those differ when deciding whether to write.

**Filters are values, not expression templates.** `FilterExpr` owns every
operand it compares against, so an expression built from a local string still
works after that string dies. The node set is a closed `variant`, which is what
lets the same value filter in memory today and lower to a tmux `-f` expression
later.

**Fields are typed.** `pane::active.starts_with("x")` does not compile,
because a flag field offers no string operations, and neither does
`pane::width.contains("8")` — a size compares as a number, where 9 is less than
10 rather than after it.

**One failure type.** Every call reports `CommandFailure`, including the
factories, so `Server::from_env().and_then(...)` composes rather than failing
to compile on a mismatched error. Every error enum has a `to_string`.

The one exception is deliberate, and follows from a rule worth stating: a
call reports the failure type its *result* speaks. `Server::control` hands
back a `Connection`, whose own surface reports `ProtocolError`, so that is
what it reports too — a doorway that disagreed with the room would only move
the conversion one line later. `Server::over_control` hands back an ordinary
`Server`, and reports `CommandFailure` like everything else you can do with
one.

**Failure is a value.** Nothing throws. `CommandFailure` says whether tmux ever
ran and why it produced nothing — a rejected argument, a spawn failure, a
timeout, tmux refusing, or the object being gone — because a caller's next move
differs for each. A timeout reports `dispatched`, since tmux may already have
acted. An object that no longer exists reports `missing`, which tmux itself
does not: asked to format a window that has been killed, it prints empty fields
and exits zero.

**Cardinality takes an lvalue.** `first` and `exactly_one` return a reference
into the range you gave them, so they refuse a temporary rather than dangle at
the semicolon. Name the range; the storage you must keep alive is then visible
in your own code.

**The transport is private, and replaceable.** No process type appears in any
installed header. Everything that reaches a process goes through one private
interface, so an async or control-mode executor arrives as another
implementation of it. The test suite substitutes one: the whole public surface
runs against a scripted executor with no tmux present, which is also how the
argv each operation sends is pinned.

## Executing work

A **batch** is one tmux invocation and one fail-fast group. The commands before
a failure have already taken effect, so a failed batch is partially applied,
not rolled back, and one exit status covers the group.

A **chain** validates each step as it is added and stops at the first failure,
so a bad target is reported where it was written rather than as a tmux message
about a command you cannot see. A chain that never became valid never reaches
tmux.

A **control connection** keeps a session open and gives every command its own
reply block, which is what makes per-command attribution possible at all.
`Server::over_control(session)` returns a Server that dispatches every entity
operation that way: the same calls, without a process each. It measured about
4.6 times faster per listing; see `docs/design/control-transport.md` for what
that costs.

```cpp
libtmux::Chain chain;
chain.new_window("work", "editor").split_window("work", "editor");
server->run_chain(chain);
```

## Examples

`examples/` holds programs, not snippets. Each one runs — against the tmux you
are inside, or a private server it starts and removes — and each is a test, so
none of them can quietly stop working.

| | |
|---|---|
| `01-tour` | connect, look around, create, read the result back |
| `02-workspace` | a session, windows, splits and commands, composing no tmux arguments |
| `03-filter` | typed fields, standard ranges, and cardinality that cannot dangle |
| `04-errors` | one call per failure kind, and what each one tells you |

```console
$ cmake --build --preset cxx-dev --target libtmux_example_01_tour
```

`examples/consume` is the exception and stays one: it proves the installed
package links and deliberately never contacts tmux.

## Consumers

Two programs use the library from opposite sides, and exist to find where it is
awkward. `consumers/mcp` is a tool surface where every call arrives as untyped
strings from a model, so it exercises validation and error reporting.
`consumers/workspace` builds a described session and reads tmuxp documents,
which exercises composition — and keeps the YAML parser it needs to itself, so
the library links none of it.

### The MCP server

The tool surface also ships as a program, so an agent can drive tmux without
anything being written against the library first. It speaks the Model Context
Protocol over stdio and offers `list_sessions`, `list_panes`, `capture_pane`,
`send_text` and `new_window`.

It is not built unless asked for, because it is the one thing here that needs
a JSON parser:

```console
$ cmake -S . -B build/mcp -DLIBTMUX_BUILD_MCP_SERVER=ON -DLIBTMUX_FETCH_DEPS=ON
```

The installed program takes the tmux socket as its only argument. Without one
it uses the server it was started inside, and failing that the one tmux itself
would pick:

```console
$ libtmux-mcp-server /tmp/tmux-1000/default
```

A model supplying a bad argument and tmux refusing a well-formed request are
different answers: the first is a JSON-RPC invalid-params error, the second a
tool result the model is meant to read and act on.

### Pointing agent CLIs at a build

`tools/mcp/mcp_swap.py` rewrites the MCP configuration of every installed
agent CLI to run a chosen copy of the server, and puts the originals back. It
handles each CLI's own dialect — TOML and JSONC with their comments intact,
opencode's single argv array, Claude's user and project scopes.

Point them at a build in this checkout, seeing the change first:

```console
$ python tools/mcp/mcp_swap.py use-local --dry-run
```

Point them at another configuration's build, or at an installed copy:

```console
$ python tools/mcp/mcp_swap.py use-local --build-dir build/cxx-gcc
```

```console
$ python tools/mcp/mcp_swap.py use-local --source published
```

Put every configuration back the way it was:

```console
$ python tools/mcp/mcp_swap.py revert
```

Before writing anything it starts the server and completes one MCP
`initialize` round trip, so a binary that cannot run fails once here instead
of inside every agent that was pointed at it.

Verified by asking the clients themselves: Claude, Codex and Cursor load a
swapped config and connect to the server, and Gemini reads and parses it.
Grok and Antigravity expose no command to query their configuration, and pi
reads one only through an adapter package, so those three are written to but
unconfirmed — `detect` says so for pi.

## How it is checked

Every change runs against real tmux under six configurations: clang with
libc++, GCC with libstdc++, the C++20 build over `tl::expected`, address and
undefined-behaviour sanitizers, the thread sanitizer, and a locale that is not
UTF-8. The tmux matrix covers every supported release from 3.2a, and the
examples run as tests.

Four parsers read input the library does not choose — what tmux printed, what a
program inside a pane put in a title, a version string a distribution may have
patched, and whatever arrives on a control socket. Each has a fuzz harness and
a checked-in corpus:

```console
$ cmake -S . -B build/fuzz -DLIBTMUX_BUILD_FUZZERS=ON -DLIBTMUX_CXX_STANDARD=20
```

## Compatibility

tmux 3.2a and newer, matching the Python package. `Version` orders releases
correctly: `3.7 < 3.7a < 3.7b`, `next-3.8` precedes `3.8`, and `master` sorts
above every numbered release. Continuous integration builds and tests against
every supported version.

C++23 is the baseline. `LIBTMUX_CXX_STANDARD=20` substitutes pinned
`tl::expected`; see `docs/design/cxx20-fallback.md`. The two builds carry
different ABI namespaces, so mixing their objects is a link error rather than
memory corruption.

## Reference

`docs/api.md` lists every public type and call with the prose from its header.
It is generated, and continuous integration fails if it drifts:

```console
$ python3 tools/docs/api_index.py --include include/libtmux --output docs/api.md
```

## Design notes

- `docs/bakeoffs/entity-behavior/scorecard.md` — the five ways an entity could
  have reached tmux, and what measuring them settled
- `docs/design/control-transport.md` — dispatching entities over one open
  connection, and what the protocol asks for in return
- `docs/design/cxx20-fallback.md` — why the fallback exists and what it costs
- `docs/design/engine-ops-study.md` — what the Python operations experiment
  taught this library, and what was declined
- `docs/evidence/library-review.md` — the adversarial reviews and their findings
- `docs/evidence/prerelease-audit.md` — what eight independent passes over the
  package found before its first release, and what was wrong with some of it
- `schema/filter-expression-v1.schema.json` — the lowered filter expression a
  JSON integration targets; the core ships no serializer
