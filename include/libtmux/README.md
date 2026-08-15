# The public headers

If it is not in this directory, it is not the contract. Everything under
[`src/`](../../src/README.md) is free to change without notice; everything here
is what a consumer may depend on.

```cpp
#include <libtmux/libtmux.hpp>
```

One include is enough — [`libtmux.hpp`](libtmux.hpp) pulls in the rest. The
individual headers are listed below for reading, not because you have to pick
among them.

**[Generated API reference →](../../docs/api.md)** — every public type and call
with the prose from its header, regenerated on every build so it cannot drift.

## What is where

### Reaching tmux

| Header | Holds |
|---|---|
| [`server.hpp`](server.hpp) | `Server` — the entry point. `from_env`, `at_socket_name`, `at_socket_path`, `at_default` |
| [`entities.hpp`](entities.hpp) | `Session`, `Window`, `Pane`, `Client`, `Buffer`, and the option structs their creators take |
| [`socket.hpp`](socket.hpp) | How a server is addressed, and what makes an address valid |
| [`target.hpp`](target.hpp) | The id-based targets every operation is addressed by |
| [`command.hpp`](command.hpp) | `CommandFailure`, `FailureKind`, `DispatchPhase` — the one error type |

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
| [`batch.hpp`](batch.hpp) | One tmux invocation, one fail-fast group |
| [`chain.hpp`](chain.hpp) | Validated as it is built; a bad target never reaches tmux |
| [`control.hpp`](control.hpp) | `Connection` — one held-open connection, a reply block per command |
| [`keys.hpp`](keys.hpp) | Key names, kept apart from the text a pane is sent |
| [`options.hpp`](options.hpp) | Options at the scope that holds them, and what "inherited" means |

### Machinery

| Header | Holds |
|---|---|
| [`expected.hpp`](expected.hpp) | `expected` — `std::expected`, or pinned `tl::expected` under C++20 |
| [`abi.hpp`](abi.hpp) | The inline namespace that keeps the two builds from linking together |
| [`version.hpp`](version.hpp) | `Version`, `parse_version`, `is_supported` — tmux release ordering |
| [`lowering.hpp`](lowering.hpp), [`lowered_node.hpp`](lowered_node.hpp) | Turning a filter into something another process could evaluate |
| [`legacy_lookup.hpp`](legacy_lookup.hpp) | Names the Python library uses, mapped to what they are here |

## What the headers promise

**Nothing throws.** Every call returns `expected<T, CommandFailure>`. A `throw`
from this library would be a bug, not an error path.

**No transport type appears here.** Nothing in this directory names a process,
a pipe or a file descriptor, which is what lets the implementation behind it be
replaced — and what lets the suite run the whole public surface against a
scripted executor with no tmux present.

**Fields are typed.** A flag offers no string operations and a size compares as
a number, so the mistakes those prevent are build errors rather than wrong
answers. [`tests/compile/`](../../tests/compile/) proves each refusal stays one.

**Two ABIs, on purpose.** The C++23 and C++20 builds live in different inline
namespaces, so linking an object from one against a library built the other way
fails at the link step instead of corrupting memory at run time.

## Related

- [The library](../../README.md) — installing it, and what using it looks like
- [`docs/api.md`](../../docs/api.md) — the generated reference
- [`src/`](../../src/README.md) — what implements this, and may change freely
- [`tests/`](../../tests/README.md) — where each promise above is pinned
