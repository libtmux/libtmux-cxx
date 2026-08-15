# libtmux C++ bakeoff and rewrite design

This design defines how `libtmux` gains a C++ library. Executable bakeoffs close
the Python parity surface and settle isolated design questions before one
minimal C++23 production rewrite under `cxx/`.

The common path stays familiar: you construct a `Server`, traverse
`Session` -> `Window` -> `Pane`, and call tmux operations through those
objects. Advanced query, raw-command, and backend seams remain opt-in.

## Outcomes

The finished tree must provide:

- One compiled CMake target and alias, `libtmux::libtmux`.
- A dependency-free C++23 core and a documented, tested C++20 compatibility
  package whose sole compatibility dependency is
  [`tl::expected` `v1.1.0`](https://github.com/TartanLlama/expected/releases/tag/v1.1.0).
- Copyable `Server`, `Session`, `Window`, `Pane`, and `Client` values with
  strong identities and explicit refresh behavior.
- Practical parity with the Python API and its tmux compatibility range.
- Explicit owning snapshots, C++20 range composition, typed query
  expressions, relation predicates, and non-throwing cardinality helpers.
- An opaque synchronous execution seam that can host subprocess or future
  control-mode backends without changing entity types.
- FetchContent, vcpkg, and installed-package consumption.
- Real-tmux GoogleTest integration through `ScopedTmuxServer`.
- Pinned formatting, curated static analysis, warnings-as-errors, and
  sanitizer CI for first-party code.
- Retained bakeoff evidence, a clean implementation rewrite, and independent
  architecture and line reviews.

Linux is the blocking support target. macOS and WSL remain portability targets
until their process, fixture, and downstream-consumer gates run in CI. Native
Windows is outside scope because tmux is not a native Windows program.

## Source baseline

The parity ledger reads two Python surfaces:

- The released [`v0.62.0` API](https://github.com/tmux-python/libtmux/tree/v0.62.0/src/libtmux).
- The development API studied for this design at
  [`c4a980b`](https://github.com/tmux-python/libtmux/tree/c4a980b/src/libtmux),
  followed by the checked-out API at generation time.

The release snapshot gives the work a durable compatibility floor. The
development snapshot detects new public symbols while the C++ port is in
flight. The manifest records the release tag, development commit and tree,
generator version, and whether the source tree was clean. Generation reads
committed objects only; a separate drift check rejects unclassified
working-tree changes. No symbol disappears because it lives in an `_internal`
module when the documented API or a public return type exposes it.

The source study covers the Python hierarchy and its supporting subsystems:

- [`Server`](https://github.com/tmux-python/libtmux/blob/v0.62.0/src/libtmux/server.py),
  [`Session`](https://github.com/tmux-python/libtmux/blob/v0.62.0/src/libtmux/session.py),
  [`Window`](https://github.com/tmux-python/libtmux/blob/v0.62.0/src/libtmux/window.py),
  [`Pane`](https://github.com/tmux-python/libtmux/blob/v0.62.0/src/libtmux/pane.py),
  and [`Client`](https://github.com/tmux-python/libtmux/blob/v0.62.0/src/libtmux/client.py).
- Command execution, targets, versions, and shared behavior in
  [`common.py`](https://github.com/tmux-python/libtmux/blob/v0.62.0/src/libtmux/common.py).
- Format acquisition and parsing in
  [`neo.py`](https://github.com/tmux-python/libtmux/blob/v0.62.0/src/libtmux/neo.py).
- Snapshot filtering and cardinality in
  [`query_list.py`](https://github.com/tmux-python/libtmux/blob/v0.62.0/src/libtmux/_internal/query_list.py).
- Environment, options, hooks, sparse arrays, testing helpers, and control
  support in
  [`options.py`](https://github.com/tmux-python/libtmux/blob/v0.62.0/src/libtmux/options.py),
  [`hooks.py`](https://github.com/tmux-python/libtmux/blob/v0.62.0/src/libtmux/hooks.py),
  [`sparse_array.py`](https://github.com/tmux-python/libtmux/blob/v0.62.0/src/libtmux/_internal/sparse_array.py),
  and
  [`pytest_plugin.py`](https://github.com/tmux-python/libtmux/blob/v0.62.0/src/libtmux/pytest_plugin.py).

The POSIX runner study also checks the standard library's subprocess contract
and implementation patterns in CPython
[`subprocess.py`](https://github.com/python/cpython/blob/3479e45/Lib/subprocess.py)
and
[`_posixsubprocess.c`](https://github.com/python/cpython/blob/3479e45/Modules/_posixsubprocess.c).
Those sources inform behavior; the C++ library does not depend on CPython.

`neo.py` does not become `neo.hpp`. Its schema, parsing, acquisition, and
refresh responsibilities move into generated field metadata, snapshot
acquisition, format parsing, and entity construction.

## Program structure

The work proceeds through four evidence gates.

### Close the contract

A generated parity manifest classifies every observed Python module-level
function, constant, type alias exposed through a signature, protocol, class,
method, property, parameter, default, return shape, exception, enum, format
field, option, hook, fixture, and public helper. It also records observable
equality, representation, iteration, indexing, hashing, and context-management
behavior. Each entry is implemented, adapted, or excluded.

Exclusion is limited to Python-runtime artifacts that cannot exist in C++:
import mechanics, Python typing helpers, pytest registration mechanics, and
`neo.py` as a module name after its capabilities are consolidated. Excluding a
functional capability requires explicit user approval. An adapted entry must
preserve the capability and observable behavior; every semantic deviation must
be named, tested, and explicitly approved. A reviewed explanation alone cannot
classify missing work as complete.

Implemented and adapted entries name:

- The C++ symbol.
- Compile probes.
- Behavioral tests.
- Documentation and executed-example IDs.
- Error and warning behavior.
- Applicable tmux versions.
- Below-boundary and at-boundary tests for version-gated behavior.

Every adapted entry additionally records the exact semantic delta,
differential-oracle ID, and approval evidence. Every excluded entry records the
inapplicability proof and approval evidence. Pending approval is a manifest
state, not a completed classification.

Completion requires no unclassified or required-but-unimplemented entries.

### Run isolated bakeoffs

Each bakeoff changes one architectural axis. Contenders implement the same
public exercise and run through the same hard gates. Correctness failures
disqualify a contender before measurements influence the decision.

### Select and specify the grafted design

The winner supplies the base. Useful properties from other contenders are
grafted only when the common tests prove they compose without weakening the
winner. A focused follow-up bakeoff runs when material uncertainty remains;
there are no performative spikes for settled choices.

### Rewrite and prove the final library

Specifications, parity manifests, acceptance tests, and findings remain.
Spike implementations are removed. The final `cxx/` implementation is written
against the retained contracts and must pass the full completion matrix.

## Public architecture

The expected final shape is one compiled target. Query templates are public
headers within that target; POSIX process code, parsers, and mutable connection
state stay compiled and private.

```text
Server / Session / Window / Pane / Client
             |
       pure command lowering
             |
      opaque ConnectionState
             |
     synchronous backend seam
             |
        subprocess tmux

Snapshot<T> + immutable relation graph
             |
   FilterExpr<T> / C++20 ranges
             |
          no I/O
```

### Entity values

Each entity contains:

- A validated strong identity such as `SessionId`, `WindowId`, or `PaneId`.
- Logically immutable cached fields from its most recent acquisition or
  refresh.
- Shared opaque connection identity.
- A complete immutable relation graph for every public relation-capable value.

The graph stores internal scalar records and adjacency by strong ID, never
public entity objects, so entity-to-graph ownership cannot form a cycle.
Snapshots share that graph with their rows. Acquisition factories and
mutation-returning methods complete a relation capture before returning an
entity; acquisition failure cannot produce a partially loaded public value.

If tmux applies a mutation and the following entity hydration fails, the method
returns `MutationApplied` rather than an ordinary failure. It carries the raw
command result, known stable identity, hydration error, and `applied` delivery
state. No incomplete entity escapes, and automatic or caller-guided retry must
not treat this outcome as an unapplied request.

Entity copies share the connection and immutable graph but own their cached
record. `refresh()` mutates only its receiver by replacing the record and
complete graph after successful acquisition; failure leaves the old state
intact. Other copies and existing snapshots do not change. Cached fields have
no public setters, but the entity object itself remains assignable. Concurrent
const access is safe; mutating and reading the same entity object requires
external synchronization.

`ConnectionConfig` contains the tmux binary, requested `-L` or `-S` selector,
configuration file, environment, and backend choice; it does not define entity
equality. `ServerEndpointKey` canonicalizes either selector to the resolved
socket endpoint. `Server` equality uses that endpoint, so different binaries,
configuration files, or selector forms that reach the same endpoint compare
equal.

Child equality adds a server-incarnation fingerprint and stable tmux ID. The
fingerprint includes resolved socket-file identity and tmux `pid` and
`start_time` handshake fields. Independently created connections to the same
live server compare consistently, while an old child cannot compare equal to
an ID reused after server restart. The parity manifest records this deliberate
hardening from Python's ID-only child comparison. Destruction has no tmux side
effect.

Complex Python keyword surfaces become named request values. An omitted flag
uses `std::optional<bool>` whenever omission differs from `false`. Raw target
strings remain available beside validated target and ID types.

### Connection and execution

`ConnectionState` owns the tmux binary, socket selection, configuration,
capability snapshot, diagnostic sink, and private synchronous backend. Those
choices do not appear in entity template parameters or concrete member types.

Calls through one shared connection are thread-safe. Each request completes
exactly once and receives only its own result. A backend may run independent
subprocess requests concurrently; a control-mode backend must serialize frame
writes and demultiplex replies. No ordering is promised between independent
mutations.

Diagnostics use a per-connection FIFO queue with at most one active callback.
Callbacks run after connection and backend locks are released; a reentrant
callback enqueues further diagnostics without recursive invocation. A callback
exception is caught, the sink is disabled, and a nonthrowing sink-failure state
is retained through `Server::diagnostic_status()`. Diagnostic failure never
changes the tmux operation result and no callback runs from a `noexcept`
destructor.

Public `CommandRequest` and `CommandResult` values are inert. A request
describes one tmux command after connection-global arguments; it is transport
IR, not a semantic operation. A result retains argv, a process termination
value, normalized stdout lines, and normalized stderr lines. Process
termination distinguishes a normal exit code from signal termination.
Successfully executing tmux produces a `CommandResult` even when tmux exits
nonzero.

The raw runner preserves bytes and keeps stdout and stderr separate. The
compatibility layer performs Python's UTF-8/backslash replacement and line
normalization, including the established `has-session` behavior. The default
POSIX backend uses argv directly, never a shell, and drains stdout and stderr
concurrently before waiting for the child. Spawn or pre-exec failure is a typed
error. A deadline failure terminates the runner's isolated process group,
drains remaining pipe data, and reaps the direct child before returning a
timeout error with dispatch phase, delivery certainty, and partial output.
Post-kill pipe draining has its own deadline. If an escaped descendant retains
a descriptor, the runner closes its remaining descriptors, marks output as
truncated, and still completes direct-child reaping instead of waiting for EOF
forever.

Command arguments carry an optional sensitivity marker. Named environment and
option operations mark secret-bearing values, and diagnostics render only a
redacted argv. Raw-command callers can mark sensitive arguments explicitly.
No logger or formatter prints the retained raw argv implicitly.

The first release remains synchronous. A future control-mode implementation
can satisfy the same synchronous backend contract, while an additive async
facade can reuse `CommandRequest` and semantic results. Semantic operation and
plan types remain an additive layer above command lowering; they retain
operation identity, result capture, dependencies, and terminal status before
lowering to `CommandRequest`. Entity types do not depend on that layer or an
executor.

Independent requests must never be conflated with tmux semicolon groups. tmux
deletes later commands in a failing group, so control-mode experiments must
not wait for result blocks that can never arrive. An observed command block may
receive an exact result, a command deleted after a known failure is `skipped`,
and ambiguous attribution is `unknown`; no layer fabricates per-command
success or failure.

## Snapshot and query contract

### Acquisition

A collection accessor performs all tmux I/O before returning:

1. Execute the required list commands through the connection.
2. Preserve raw output, then apply compatibility normalization.
3. Parse typed rows and winlink edges.
4. Materialize complete adjacency for public relation fields.
5. Return an immutable `Snapshot<T>`.

`Snapshot<T>` owns its row storage and a normalized immutable relation graph.
A loaded empty relation is distinguishable from acquisition failure; an
unloaded public relation is not representable. Lenient acquisition returns an
empty snapshot on any tmux failure rather than a partial graph.

Relation-complete collection acquisition may need more commands than Python's
base list accessor. If the base list succeeds but required relation enrichment
fails, the lenient accessor returns an empty snapshot and the checked accessor
returns the enrichment error plus successful command evidence; neither returns
a partial graph. This widened failure boundary is a named parity adaptation
that requires explicit approval and a forced-enrichment-failure differential
test.

Snapshots record the connection capability fingerprint and per-table capture
sequence. tmux offers no transaction across list commands, so cross-table
skew is explicit provenance. A later tmux mutation never changes an existing
snapshot. Acquiring or refreshing produces new cached state.

Views borrow through the exact snapshot object used to construct them and must
not outlive or be used after assignment to that object. View construction,
predicate calls, cardinality helpers, and iteration perform no tmux I/O.

Snapshots share immutable heap-stable storage. Copy and move construction both
share that storage, and the source of a move remains unchanged and valid. A
source-bound standard `ref_view` therefore remains tied to its original source;
it is never reparented to the destination. Copy or move assignment invalidates
views bound to the destination's replaced state but not views bound to the
unchanged source. Destruction invalidates views bound to that snapshot object.
Compile and sanitizer tests cover each case.

### Typed expressions

`FilterExpr<T>` is a value, not an expression template. Every node is a
`std::variant` alternative and every literal is owned. Expression payloads use
`std::string`, never `std::string_view`.

A generated `constexpr` field handle records:

- Entity and value type.
- Supported operations.
- Stable field or relation ID.
- tmux format token and scope.
- Minimum tmux version.

The public syntax includes:

```cpp
auto expr = pane::command.starts_with("nv") && pane::active;
```

`&&`, `||`, and `!` build owned AST nodes. Invalid operations, mixed entity
expressions, and predicates applied to the wrong row type fail compilation.
Evaluation short-circuits Boolean nodes even though construction cannot.

The operator inventory preserves `eq`/`exact`, `iexact`, `contains`,
`icontains`, `startswith`, `istartswith`, `endswith`, `iendswith`, `in`, `nin`,
`regex`, and `iregex`. Regex nodes own source text and flags. A
`RegexPattern::compile` factory validates patterns and returns a typed error, so
an invalid pattern cannot hide an exception inside expression construction or
evaluation. The query compatibility study differentially tests Python and C++
over ASCII, UTF-8 case conversion, membership shapes, anchors, groups, classes,
escapes, and invalid patterns. Any standard-library regex or case-folding
deviation is an adapted parity entry requiring explicit approval before AST
selection.

`FilterExpr<T>` satisfies `std::predicate<FilterExpr<T>, const T&>`. Direct
invocation and standard views therefore work without an adapter:

```cpp
auto filtered = panes | std::views::filter(expr);
```

`tmuxq::matching(expr)` returns the equivalent standard filter closure and
owns its expression. It composes with other standard views. The query
vocabulary lives in `libtmux::tmuxq` so a caller can import the adaptors
without the whole library, and `libtmux::matching` names the same function
rather than a second one.

Missing scalar fields fail their predicate. Boolean negation then inverts the
result normally. Relation expressions read only the snapshot graph:

- `any_of(empty)` is `false`.
- `all_of(empty)` is `true`.
- `none_of(empty)` is `true`.
- A missing to-one relation fails `is`.

### Cardinality and generic ranges

Generic downstream algorithms accept any `std::ranges::input_range` whose
reference type is `const T&`. They take ordinary predicates; they require a
`FilterExpr<T>` only when inspecting or translating its AST. Compile probes
prevent generic APIs from imposing either `Snapshot<T>` or `FilterExpr<T>`.

`cardinality_result<T>` is the package-selected expected type with
`CardinalityError` as its error value.

Reference-returning cardinality helpers additionally require a
`std::ranges::forward_range` with stable `const T&` references. `first` returns
`std::optional<std::reference_wrapper<const T>>`. `exactly_one` returns
`cardinality_result<std::reference_wrapper<const T>>`; its
`CardinalityError` distinguishes `NoMatch` from `Multiple`, and it examines at
most two elements. The stronger iterator category is required because advancing
a single-pass iterator may invalidate its first reference. This is a safety
adaptation to the requested reference signature and requires approval in the
written-spec review.

For a general input range, `first_value` returns
`std::optional<std::ranges::range_value_t<R>>`, and `exactly_one_value` returns
`cardinality_result<std::ranges::range_value_t<R>>`. These owning overloads
preserve single-pass range support without claiming unsafe reference lifetime.

Reference-returning helpers accept lvalue ranges and explicitly borrowed
rvalues. They reject temporary owning snapshots, containers, and unsafe
prvalue-producing ranges at compile time. A caller binds an ordinary filtered
view before requesting a reference from it.

### Edge parser and serialization

The typed API does not parse lookup strings. A compatibility edge parser
splits on the first `=`, resolves the last `__` suffix through generated
metadata, and owns the right-hand side.

`name__contains=` means `contains` with an explicit empty string. A missing
`=`, empty field, unknown field, unknown suffix, or malformed repeated suffix
is a parse error. There is no silent fallback to exact lookup.

The core exposes a read-only AST visitor. A separate opt-in JSON header lowers
the AST through a serializer concept. The language-neutral canonical schema is
committed under `cxx/schema/` and owns the schema identifier, integer major
version, entity kinds, closed node tags, field IDs, relation IDs, regex flags,
and scalar encodings. An incompatible representation change creates a new
major schema; version-one tags never change meaning.

The C++ header is an encoder only. Serializer errors propagate in the
serializer's own result type; the core gains no JSON dependency or exception
policy. Cross-language golden documents are checked against the canonical
schema and the C++ serializer event stream. A decoder or round-trip contract is
future work and is not implied by lowering.

Snapshot filtering has no pushdown hook. A future
`Server::query_sessions(expr)` may compile supported nodes to tmux `-f` before
materialization and evaluate residual nodes locally. That path remains
separate from `matching` and direct invocation.

## Error and diagnostic contract

Recoverable APIs use `libtmux::result<T>`. The default C++23 package uses
`std::expected`. The separately built C++20 package uses a pinned
`tl::expected` `v1.1.0`, including recorded source, archive hash, and license.

These packages have distinct ABI namespace and binary identities. Every public
type, not only `result`, lives in the package's inline ABI namespace. Generated
configuration and exported target metadata select exactly one identity, and a
mismatched consumer fails to link rather than creating an ODR collision. The
two variants cannot install over one another or be combined in one build tree.
Within either package, consumers use the single alias `libtmux::libtmux`.

The error value distinguishes:

- Request validation.
- Process spawn, pre-exec, and pipe failures.
- Timeout before dispatch and uncertain delivery after dispatch.
- Mutation applied but entity hydration failed.
- tmux command failure.
- Protocol and decoding failure.
- Missing object on a live server.
- Dead, missing, or inaccessible server connections.
- Unsupported version or capability.
- Query parsing and cardinality.

A timeout after possible delivery has `unknown` delivery. Mutating requests
are never replayed automatically.

Raw `cmd()` preserves process results. Higher-level methods translate stderr,
exit status, and tmux messages according to the Python parity manifest.
Warn-and-continue behavior reports through an optional diagnostic sink rather
than global logging or stderr.

List-shaped accessors return an empty snapshot on every tmux failure by
default. Checked collection variants preserve the typed error.
`Server::is_alive()` and `Server::check_alive()` distinguish an empty server
from an unreachable one without changing list-accessor behavior. Raw search
APIs retain loud tmux-format failures and remain distinct from local snapshot
filtering.

## Bakeoff design

### Transport ownership

Opaque, non-templated entity types are an approved invariant. Three working
vertical slices therefore compare private backend ownership behind the same
public `ConnectionState` PIMPL:

- An internal abstract backend owned by `std::unique_ptr`.
- A manual type-erased backend with a private function table.
- A closed private `std::variant` of compiled backend implementations.

Public virtual backends and template-policy entity types are not contenders:
both would make transport choice part of the downstream entity ABI and violate
the executor-independent public contract.

Each slice starts an isolated tmux server, creates a session, lists and parses
it, filters a snapshot, exercises cardinality, reports launch and tmux
failures separately, and builds through a basic downstream consumer.

The bakeoff selects the private mechanism by diagnostics, compile cost,
testability, extensibility, and runtime cost. It cannot expose virtual bases,
backend alternatives, or transport policy parameters in public entity types.

### Query compatibility

Before AST storage is selected, a focused differential study classifies every
Python lookup operator, operand shape, missing-value case, Unicode case rule,
and regex behavior. The study supplies shared golden inputs to Python and each
C++ evaluator candidate. It must either prove matching behavior or name an
adaptation for explicit approval; it cannot silently substitute C++ regular
expression or locale semantics.

### AST storage

On the selected transport shape, three query implementations compare:

- A flat postorder variant arena with child indices.
- A recursive owning variant tree.
- An immutable shared variant DAG.

All expose the same `FilterExpr<T>` surface. The flat arena is the research
favorite because it gives deep value semantics, stable node IDs, and direct
serialization without recursive ownership. The recursive contender supplies
the readability benchmark; the shared DAG must justify allocation and
reference-count costs with measured gains.

### Relation storage

A relation bakeoff runs only if the AST exercise does not settle storage and
lifetime behavior. Any contender must preserve exact
`bool operator()(const T&)` evaluation, complete loaded-versus-empty semantics,
linked-window edges, and zero-I/O iteration.

### Hard gates

Each contender passes the gates for its changed axis plus the common real-tmux
vertical slice. The selected transport and query graft must pass the complete
suite before the clean rewrite:

- Real-tmux acquisition and mutation-after-snapshot tests.
- Forced post-mutation hydration and relation-enrichment failures, including
  applied identity, retry prohibition, lenient empty results, and checked error
  evidence.
- Transport-call counters proving zero I/O during query operations.
- Compile-fail tests for invalid fields, operators, entities, relation kinds,
  temporary owners, and prvalue ranges.
- ASan/UBSan lifetime tests.
- Owned-literal copy, move, container, and return-from-function tests.
- Snapshot move construction, copy/move assignment invalidation, and owning
  cardinality over single-pass ranges.
- Empty-relation laws and to-one relation tests.
- Every lookup operator, invalid regex, UTF-8 case, `name__contains=`, and
  malformed parser cases.
- JSON schema, cross-language golden, unknown-version, and serializer-failure
  tests. Unknown-version behavior is tested in the schema oracle, not a C++
  decoder.
- Spawn failure, pre-exec failure, nonzero exit, signal termination, timeout,
  escaped pipe holders, redaction, and large stdout/stderr tests.
- Same-connection concurrent reads and mutations, result attribution,
  reentrant and throwing diagnostics, and deterministic teardown under
  contention.
- A control-mode graft probe with interleaved notifications and a failing
  command group that cannot hang.
- An engine-ops adapter probe that preserves semantic operation identity,
  capability-aware lowering, grouped success/failure/skipped/unknown
  attribution, terminal statuses, and result capture without putting an
  executor in entity types. At least one plan consumes an ID captured by an
  earlier operation. Its report records the engine-ops commit, tree, and clean
  source status so later drift cannot inherit the claim.

### Measurements and decision

After hard gates pass, the scorecard records:

- Clean and incremental compile time.
- Public-header parsing time.
- Binary size.
- AST composition and evaluation allocations.
- Diagnostics for representative invalid code.
- Source and template footprint.
- Ease of adding a recording backend and control-mode adapter.
- Build-tree and downstream-consumer behavior.

No fixed performance threshold selects a winner before measurements. The
decision report names the winner, grafts, rejected trade-offs, and the evidence
for each. A fresh adversarial reviewer challenges the report before the final
architecture is accepted.

## Parity proof

### Generated manifest

Standard-library Python tooling reads source through `git show` and `ast`; it
does not import or check out another revision. It produces frozen release and
current-development observations. The generated manifest embeds the release
tag, full development commit and tree IDs, generator version, and clean-source
policy. It also embeds the exact Python parity-input paths and their blob or
subtree identities; `cxx/`, unrelated documentation, caches, and build outputs
are outside that drift boundary.

The generator extracts:

- `__all__`, documented modules, classes, methods, properties, signatures,
  defaults, overloads, and deprecations.
- Inherited environment, option, and hook operations.
- Format fields, scopes, and version gates.
- Enums and exception inheritance.
- Option, hook, and sparse-array schemas.
- Public testing helpers and fixtures.

A reviewed mapping records the C++ counterpart and contracts. Generation into
a temporary directory must reproduce committed manifests and generated headers
byte for byte. A separate check compares only recorded Python parity-input
paths in the working tree with their embedded identities. Any development API
drift in that boundary fails until classified.

### Behavioral differential tests

Python and C++ scenarios run against separate named sockets using the same
tmux executable. Canonicalization removes only unstable IDs, PIDs, timestamps,
socket paths, and ordering the Python API does not guarantee.

The comparison covers:

- Object rows and winlink edges.
- Raw argv and normalized command results.
- Return shapes and optional values.
- Warning and error categories.
- Target resolution and stale-object behavior.
- Environment, options, hooks, and sparse arrays.
- Version and capability decisions.

## Real-tmux test harness

`ScopedTmuxServer` is a move-only test-support utility used by GoogleTest. It
owns the actual server process and its private socket directory; it is not an
installed public API.

Construction:

1. Create a private short directory plus a unique socket selector.
2. In path mode, spawn `tmux -D -u -S <path> -f /dev/null`. In name mode, set a
   fixture-owned `TMUX_TMPDIR` and spawn with `-L <name>`. Never change `HOME`.
3. Expose the socket mode, optional socket name, and resolved exact socket path.
4. Remove only `TMUX` and `TMUX_PANE` from the child environment.
5. Poll readiness against an absolute monotonic deadline.
6. Create a deterministic detached session and route every command through
   the same immutable connection.

The `noexcept` destructor attempts `kill-server`, waits with a bound, sends
TERM and then KILL only to the owned PID if required, reaps it, and removes
only fixture-owned paths. Partial construction uses the same cleanup path.
Teardown diagnostics never mask the primary test failure.

Tests cover both `-L` and `-S` connections, name/path accessors, parallel
fixtures, assertion and constructor failure, server self-exit, stale socket
state, ambient tmux variables, long socket paths, a hung child, and leak-free
cleanup.

## tmux compatibility matrix

Blocking cells cover tmux `3.2a`, `3.3a`, `3.4`, `3.5`, `3.6`, exact `3.7`,
`3.7a`, and `3.7b`. Exact `3.7` is required because the Python implementation
contains behavior that does not apply to `3.7a`. The `master` cell reports
upstream drift but does not block.

Each stable tmux build is pinned to an immutable source identity and archive
hash. The informational `master` job resolves upstream HEAD once, records that
commit and archive hash, and uses only that source for the run. The selected
binary path is passed explicitly and verified with `tmux -V` before testing.
Version handling retains raw text and a conservative comparable value because
tmux versions are not SemVer. Capability gates, not numeric ordering alone,
select behavior.

## CMake and distribution

The final library uses CMake 3.25 or newer and target-scoped compile features.
It never sets a global C++ standard or propagates first-party warning flags.

The package provides:

- `libtmux` plus alias `libtmux::libtmux`.
- Correct build and install include interfaces.
- `GNUInstallDirs` destinations.
- Exported targets, `libtmuxConfig.cmake`, and a compatible version file.
- Static or shared production through `BUILD_SHARED_LIBS`.
- Tests, examples, install rules, warnings-as-errors, clang-tidy, and
  sanitizers enabled by explicit options and top-level-aware defaults.

GoogleTest is test-only and system-first, with a pinned FetchContent fallback
when tests explicitly permit downloads. Configuring or building the production
library never requires network access.

Isolated consumers must configure, compile, link, run, install, and relocate
through:

- `add_subdirectory(cxx)`.
- `FetchContent_MakeAvailable` from a local source checkout with
  `SOURCE_SUBDIR cxx`.
- A staged install followed by `find_package(libtmux CONFIG REQUIRED)`.
- The same consumer after moving the staged prefix.
- A vcpkg overlay port in manifest mode.

The matrix covers static and shared libraries, GCC and Clang, C++23, and the
C++20 fallback. Installed files contain no source-tree includes, build paths,
unexpected RPATHs, or missing transitive headers.

The C++20 package installs the pinned compatibility header and fixes its ABI
identity in generated configuration. The C++23 package has a different output
name and ABI namespace. A consumer cannot silently switch representations, and
install-time checks reject a prefix containing the other package identity.

## Tooling and CI

The repository pins
[`clang-format`](https://github.com/llvm/llvm-project/tree/llvmorg-18.1.3/clang/tools/clang-format)
and
[`clang-tidy`](https://github.com/llvm/llvm-project/tree/llvmorg-18.1.3/clang-tools-extra/clang-tidy)
`18.1.3` in its tool manifest. The same binaries run locally and in CI; a
floating major version does not satisfy this requirement.

The formatter owns source layout. clang-tidy uses a reviewed check list rather
than every available check. Warnings-as-errors apply only to first-party
targets. Sanitizer flags stay private.

CMake presets cover:

- Development and release builds.
- C++20 compatibility.
- ASan plus UBSan.
- clang-tidy.
- Install and downstream consumer tests.

CI runs:

- GCC and Clang builds.
- clang-format verification.
- clang-tidy.
- Warnings-as-errors.
- ASan and UBSan.
- Real-tmux GoogleTest and Python/C++ differential scenarios.
- The stable tmux matrix and informational `master` cell.
- Static, shared, install, relocation, FetchContent, and vcpkg consumers.
- Python formatting, linting, type checking, tests, and documentation.

Linux is blocking. A future macOS support claim requires AppleClang plus the
process, real-tmux fixture, install, relocation, and consumer cells. A future
WSL support claim requires the same evidence in WSL rather than inference from
Linux.

Every public C++ API has a compiled and executed example. CTest treats examples
as tests, providing the C++ equivalent of the Python project's executable
doctest rule. The parity manifest maps every public symbol to its example and
documentation IDs, and a coverage check rejects missing or duplicate mappings.

## Clean rewrite

Spike code lives in an isolated subtree and is excluded from installation and
production exports. Reports are public-safe evidence, not user-facing release
narrative.

Before final implementation:

1. Freeze the approved public contract and acceptance tests.
2. Retain parity manifests, schema, design specifications, and bakeoff reports.
3. Remove every spike implementation.
4. Verify no production `cxx/` source remains.
5. Implement the selected design afresh against the retained contracts.

The final audit rejects experimental includes, source paths, symlinks,
submodules, vendored binaries, build outputs, undeclared generated files, and
copy/rename provenance from a tracked spike implementation. Two fresh
out-of-tree builds must leave the checkout clean and produce identical
generated headers and install manifests.

## Adversarial review

A reviewer who did not implement the contender reviews each bakeoff report and
the selected architecture. The review challenges lifetime ownership,
transport leakage, hidden I/O, public templates, ABI assumptions, packaging,
error attribution, and unnecessary surface area.

After the clean rewrite, a fresh C++ reviewer examines every changed line for:

- Ownership, moves, copies, and dangling references.
- Undefined behavior and exception safety.
- Process, pipe, signal, and timeout handling.
- Thread-safety claims and shared-state serialization.
- Header hygiene, compile cost, diagnostics, and naming.
- CMake target hygiene and downstream propagation.
- Duplicate helpers, speculative abstractions, comments without durable
  rationale, and other review-hostile noise.

Every finding is fixed or dispositioned with concrete evidence before the
completion audit.

## Planned final tree

```text
cxx/
  CMakeLists.txt
  CMakePresets.json
  cmake/
  docs/
  examples/
  include/libtmux/
    capabilities.hpp
    client.hpp
    command.hpp
    environment.hpp
    error.hpp
    hooks.hpp
    ids.hpp
    libtmux.hpp
    options.hpp
    pane.hpp
    query/
    serialization/
    server.hpp
    session.hpp
    snapshot.hpp
    version.hpp
    window.hpp
  schema/
  src/
  tests/
    support/
  tools/
  vcpkg/
```

Generated headers are committed so ordinary consumers do not need the
generator. There is no `neo.hpp`, public transport implementation hierarchy,
or separate public component target until a real downstream caller requires
one.

## Completion audit

The project is complete only when current evidence proves all of the following:

- The parity manifest has no missing required entry or unreviewed drift.
- No adapted or excluded parity entry has pending approval; each names its
  semantic delta or inapplicability proof, differential oracle when applicable,
  and approval evidence.
- Every mapped symbol has compiling API probes and passing behavior tests.
- Every public symbol has a unique documentation and executed-example mapping.
- Python and C++ differential scenarios pass across the stable tmux matrix.
- Snapshot construction and iteration obey their lifetime and zero-I/O rules.
- Query compile-fail, serialization, relation, and cardinality tests pass.
- Every Python lookup operator is implemented or has an explicitly approved,
  differentially tested adaptation.
- The process runner and `ScopedTmuxServer` pass failure, concurrency, and leak
  tests.
- The engine-ops adapter probe preserves semantic identity, dependent capture,
  honest grouped attribution, and terminal status without executor coupling.
- C++23 and C++20 builds pass with self-contained public headers.
- Exported-target and package-config audits prove no third-party production
  dependency in C++23 and only pinned `tl::expected` in C++20.
- Formatting, clang-tidy, warnings-as-errors, ASan, UBSan, GoogleTest,
  examples, documentation, and the full Python gate pass.
- FetchContent, vcpkg, installed, relocated, static, and shared consumers pass.
- Fresh builds leave the checkout clean and reproduce generated and installed
  artifacts.
- Retained reports cover every bakeoff contender, measurements, winner and
  graft decisions, adversarial dispositions, and a focused follow-up spike for
  every unresolved material ambiguity.
- Both adversarial reviews have no unresolved finding.
- No spike implementation survives in the production library.
- The blocking platform claim is no broader than the CI evidence.

Passing a smaller slice does not support a broader completion claim.

## C++ reference patterns

The implementation study may graft patterns, not dependencies, from:

- [fmt's compiled target and package exports](https://github.com/fmtlib/fmt/blob/7bce225/CMakeLists.txt).
- [Catch2's inherited CMake presets](https://github.com/catchorg/Catch2/blob/b59f4f3/CMakePresets.json).
- [spdlog's target-local warning and sanitizer helpers](https://github.com/gabime/spdlog/blob/0209b12/cmake/utils.cmake).
- [TensorStore's typed subprocess API](https://github.com/google/tensorstore/blob/a976c5a/tensorstore/internal/os/subprocess.h).
- [simdjson's standard-range-compatible containers](https://github.com/simdjson/simdjson/blob/e532d61/include/simdjson/dom/array.h).
- [FlatBuffers' separation of parser state and typed products](https://github.com/google/flatbuffers/blob/38df293/include/flatbuffers/idl.h).
- [tmux `3.7b` command parsing](https://github.com/tmux/tmux/blob/3.7b/cmd-parse.y)
  and [control-mode protocol](https://github.com/tmux/tmux/blob/3.7b/tmux.1).

These links pin the source claims. The C++ library does not import their
architectures wholesale and adds no runtime dependency on them.
