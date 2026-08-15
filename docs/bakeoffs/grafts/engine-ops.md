# Engine-ops adapter graft

## Result

The private graft can lower owned operation values, resolve captured entity
IDs by semantic producer ID, and retain one terminal status per planned
operation without adding engine types to contender entity headers. It does not
select a transport contender or make the graft a public API.

## Source boundary

The source lock fixes one repository URI, commit, tree, and eight blobs. The
materializer fetches into a disposable task-owned bare repository, rejects
ambient Git state and hostile configuration, verifies every object, and
publishes an exact read-only tree plus a canonical manifest. Configuration and
the library, adapter test, and aggregate target recheck the manifest, payload
digests, directory topology, symlink-free path, and complete entry set without
fetching. Atomic no-clobber publication requires Linux `renameat2` with
`RENAME_NOREPLACE`.

The locked paths cover the experimental engine and plan boundary. They do not
include concrete `_ops` definitions. The C++ operation fields, defaults, and
argument ordering are this graft's model, verified by lowering tests and live
tmux tests; they are not a claim of concrete Python operation parity.

## Lowering and execution

`NewSession`, `SplitPane`, `SendKeys`, and `KillSession` lower to owned command
requests. Captured targets accept only the expected field and exact ASCII
decimal entity-ID grammar. Invalid, missing, forward, self, wrong-field, and
wrong-kind references fail before dispatch. A valid dependency whose producer
does not complete is skipped.

Capture-producing operations are not grouped. For other contiguous groups, a
directly invalid member fails and otherwise valid undispatched siblings are
skipped. Exact result prefixes remain attributable after a later connection
error; unresolved suffixes are unknown. Capture bindings are installed only
from exact successful results with strict output shapes. The execution layer
assumes its private adapter preserves request order and reports coherent
attribution; it does not independently reconstruct physical fail-fast events.
The test-only process adapter executes requests separately. Group attribution
is therefore synthetic trusted-adapter contract evidence, not live proof of a
physical fail-fast command group.

## Capability and live proof

When `kill_session_group` is true, `KillSession(group=true)` emits `-g`; when
false, it omits the flag. The latter is an adaptation without Python warning-
channel parity. Only the supported branch was tested live against raw version
`tmux 3.7b\n`, binary SHA-256
`0cd875611e001f9d66c65977d499a65de2400c967b5c7788d04d43a2c9f06982`,
and path-selected sockets. The live test removes the target group while an
unrelated session and the fixture server remain. No pre-3.7 runtime behavior
was tested.

## Evidence

The Task 7 probe passes all 32 registered tests in development,
ASan/UBSan, and TSan builds. The full C++ suite passes all 223 tests in each
preset. The materializer suite passes all 35 tests. All six Task 7 translation
units use warnings as errors and nonrecovering UBSan; the sanitizer builds add
their expected instrumentation. Format, clang-tidy, Ruff, mypy, CTest metadata,
scope, and privacy checks pass.

The immutable `graft-engine-ops` development gate has digest
`cdbd23a8ba405b509e5b55ea426b9e367210af7ab9ac0eb6a10b810715cffb26`.
Its source-lock digest is the SHA-256 of the canonical semantic JSON encoding
of the source lock.

This is spike-only evidence and the graft must be removed before production
implementation. It does not select a transport, measure performance, prove
cross-version tmux behavior, or establish concrete Python operation parity.
