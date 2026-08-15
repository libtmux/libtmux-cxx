# Implementation

Nothing here is installed, and nothing here is a promise. The contract is
[`include/libtmux/`](../include/libtmux/README.md); this is what currently
satisfies it, and it is free to change.

```console
$ cmake --build --preset cxx-dev --target libtmux
```

## The transport seam

The one structural idea worth knowing before reading any of it: **no public
header names a process.** Everything that reaches tmux goes through one private
interface, and the rest of the library is written against that.

| File | Does |
|---|---|
| [`backend.hpp`](backend.hpp) | The seam itself — the only way anything here reaches tmux |
| [`backend.cpp`](backend.cpp) | The default backend: one process per command |
| [`control_backend.hpp`](control_backend.hpp), [`control_backend.cpp`](control_backend.cpp) | The other one: entity operations over a held-open control connection |
| [`process.hpp`](process.hpp), [`process.cpp`](process.cpp) | Spawning, timeouts, capture limits. The only file that knows what a pipe is |
| [`connection.cpp`](connection.cpp) | The control-mode protocol: reply blocks, notifications, the bounded buffer |

That seam is why the suite can run the entire public surface with **no tmux
present**, against a scripted executor — and why the argv each operation sends
can be pinned exactly. It is also how control mode arrived as a second
implementation rather than a rewrite.

## The rest

| File | Does |
|---|---|
| [`entities.cpp`](entities.cpp) | Session, window, pane and buffer operations, and the argv each one sends |
| [`server.cpp`](server.cpp) | Listing, addressing, and the factories |
| [`snapshot.cpp`](snapshot.cpp) | Parsing what tmux printed into the shared row store an entity indexes into |
| [`control.cpp`](control.cpp) | Control-mode types on the public side of the seam |
| [`version.cpp`](version.cpp) | Out of line so the package ships a compiled artifact, not headers alone |
| [`acquire.hpp`](acquire.hpp), [`transport_values.hpp`](transport_values.hpp) | Internal helpers, deliberately not installed |

## Things that look wrong and are not

**`snapshot.cpp` parses on a separator you will not recognise.** tmux formats
are joined with `␞` (U+241E), because it is the one character a pane title, a
command line or a path will not contain. A tab or a comma would be a bug
waiting for the first person with an unusual window name.

**Every command carries `-u`.** tmux decides whether to emit UTF-8 from the
locale, so without it a machine with a non-UTF-8 locale gets different bytes
back. There is a CI lane that runs the whole suite under one.

**A missing object is detected, not reported by tmux.** Asked to format a
window that has been killed, tmux prints empty fields and exits zero. The
library notices and reports `FailureKind::missing`, because a caller's next
move differs from "it worked and everything was blank".

## Related

- [`include/libtmux/`](../include/libtmux/README.md) — the surface this satisfies
- [`docs/design/control-transport.md`](../docs/design/control-transport.md) — why the second backend exists and what it cost
- [`tests/`](../tests/README.md) — including the scripted-executor contract tests
