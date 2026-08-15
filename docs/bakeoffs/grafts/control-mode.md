# Control-mode graft

## Result

A persistent control-mode connection can preserve reply ownership for
independent requests and honestly classify fail-fast command groups. One
request represents one semicolon group; separate calls represent independent
requests. The connection serializes complete frame writes while the reader
demultiplexes command blocks from asynchronous notifications.

## Protocol boundary

The byte parser retains begin and terminal metadata, distinguishes `%end` from
`%error`, and preserves block bodies without UTF-8 conversion. Lines shaped
like notifications inside a block remain block body because control-mode
framing does not make them independently attributable. Outside a block,
`%output` and `%extended-output` octal escapes are decoded while unknown
notification lines remain byte-exact.

Seven golden streams cover successful and failed guards, percent-prefixed block
bodies, interleaved notifications, both octal-output forms, NUL, and high-byte
data. Each stream passes at every split boundary and one byte at a time.

## Attribution and lifecycle

The real-tmux fail-fast group returns `exact/error`, `skipped`, and `skipped`.
A subsequent independent request returns `exact/end`, proving that deletion of
the failed group's suffix does not poison later ownership. A lost, timed-out,
or shut-down connection marks unresolved operations `unknown` with a connection
error and rejects later requests instead of attaching late blocks to them.

Shutdown cancels blocked writers, avoids completing a partially written command
with a detach newline, terminates the owned client when needed, and reaps it
once.

## Evidence

The probe is bound to raw version `tmux 3.7b\n` and binary SHA-256
`0cd875611e001f9d66c65977d499a65de2400c967b5c7788d04d43a2c9f06982`.
It uses path-selected fixture sockets without recording an executable or socket
path.

The immutable `graft-control-sanitize` gate passed all 18 registered tests.
Its gate digest is
`4d20cd44b9933fa0924b449b1378a9b28e02f7721350f74e5f9501b3f0ce82ea`;
the registration digest is
`8a200541682bc2d912fe1c75371c6d167e4e44ec36747004b30844810978c68f`;
and the JUnit digest is
`848503b777752884d96350cfbd01cc8a0463e89c426997aa8f064d9c3c9f2d15`.

This graft is Linux/POSIX-specific and intentionally tied to the tested tmux
binary. A parser or connection failure is terminal for that connection; the
caller must create a new connection rather than replay an ambiguously
dispatched request.

This is spike evidence, not a production or public API commitment. It does not
prove behavior across tmux versions, measure performance, or select a backend
architecture.
