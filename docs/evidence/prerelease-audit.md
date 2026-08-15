# Pre-release audit

Eight independent passes over the package — the API as a caller meets it, the
implementation line by line, the documentation, the examples, tmux behaviour
against the 3.2a and 3.7 sources, packaging, test rigour, and the decisions
already made — each followed by a reader whose job was to disprove what the
pass claimed, and then by one looking for what all eight missed.

The finding that organises the rest: everything worked, in one configuration.
Clang, libc++, a UTF-8 locale, one machine's shell. Outside that, the library
did not compile, could not read a single listing, or answered wrongly without
saying so.

## Silent wrong answers

These returned success. A caller could not tell.

**A non-UTF-8 locale broke every listing.** The separator between format values
is U+241E, and a tmux that has decided the terminal is not UTF-8 writes an
underscore instead, so no row splits. The control transport had always passed
`-u`; the subprocess transport had not. Nine of the suite's entries failed in a
C locale, four by crashing.

**Every command truncated at a megabyte.** The runner recorded that it had cut
the output and nothing read the flag. A capture of a long scrollback came back
as exactly 1,048,576 bytes with a successful result and a last line cut
mid-word — a prefix that reads like data. The two transports also disagreed:
the control connection returned the whole thing.

**Moving a window unlinked it from another session.** A window can be linked
into several sessions, so a bare `@id` is not one object. tmux resolved it to
one of its homes: the move landed in the wrong session, the link the caller
never mentioned disappeared, and `refresh` reported a different link's index.
Qualifying by session fixes the move and makes the missing case *worse* — asked
about `$0:@dead`, tmux answers with whatever that session is currently showing,
exit zero. Both need the answer's identity checked against the question's.

**An argument ending in a semicolon moved a command boundary.** tmux reads a
trailing `;` as a separator, so a value arrived truncated and, in a batch,
whatever followed it ran as its own command. The argv a two-member batch
renders, ending in `kill-server`, killed the server.

**Text beginning with a dash was refused as a flag**, so a pane could not be
sent `-foo` and a window could not be named `-name`.

**The key validator rejected keys tmux accepts.** It listed eighteen names by
hand where tmux's table has fifty: `Insert`, `Delete`, `PgUp`, `PgDn` and the
whole keypad were refused as typos. It exists to catch a typo tmux would
swallow, and was rejecting real keys instead. It also accepted control bytes,
which tmux delivers as nothing.

## Reachable undefined behaviour

**`capture_lines(*pane.capture())` was a heap use-after-free** — the spelling
the pane header recommended. The lines are views into text the call returns by
value. Clang 18 does not implement P2718R0 and `-Wdangling` says nothing;
AddressSanitizer confirms it.

**The cardinality helpers guarded the range's lifetime, not the element's.**
Over a range that produces elements on demand, `exactly_one` returned a
reference to one that had already died.

Both are now compile errors, with a probe each and the legal spellings still
building.

## Configurations that did not work

**GCC could not compile the library** — three lines where a duration's
representation is `long` on one standard library and `long long` on another.
With those spelled explicitly, GCC and libstdc++ build the library, the tests
and both consumers at `-Werror` and pass the whole suite.

**A process that ignores SIGCHLD got "No child processes" from every command,**
with nothing connecting that to the disposition it had set. Routine in a
daemon.

**The installed package could not be embedded.** Anything adding this project
with `add_subdirectory` inherited its install rules and filled its own prefix.
It declared no thread dependency while starting two threads per control
connection, shipped no license, and told CMake that any 0.x satisfied any
other.

## Tests that could pass for the wrong reason

Two tests asserted that a pane runs what it is sent by looking for the text
they had typed — which the shell echoes before running anything. They passed
with no Enter sent at all, and raced the echo, which is where an intermittent
failure came from.

The fixture kept the developer's `HOME` and `SHELL`, so a pane ran their login
shell with their configuration. An interactive zsh handed an unfamiliar HOME
opens a first-run wizard and swallows every keystroke, which reads exactly like
a library that sent nothing. The fixture now supplies a private HOME inside the
tree it already removes, and the shell POSIX guarantees.

## What did not survive scrutiny

The reader whose job was to disprove each finding killed several, and they are
recorded so they are not raised again:

- **`swap_with` needs `-d`** — inverted. In `cmd-swap-window.c`, `-d` is what
  *calls* `session_select`; adding it would regress the common case. The bug
  was in `move_to`, where the polarity is the other way round.
- **The lowering layer is unused** — every relation lowers its child through
  it, and no rule installs the schema.
- **`<filesystem>` costs a third of the umbrella's compile time** — measured at
  about one percent.
- **Relation lifetime is undocumented** — the header documents it precisely;
  only the README overclaimed.
- **A position-independent archive is required to link into a shared object** —
  did not reproduce on either toolchain here, both of which default to PIE. The
  property is set anyway, so the answer does not depend on a compiler default.

## The evidence pipeline is broken between its halves

`record-evidence` validates a gate record that `ctest_gate` has never
produced. It requires `shard`, `real_tmux`, `execution_mode`,
`evidence_ids`, `cmake_target`, `ctest_label` and `result_sha256`; the
gate emits none of those, in any commit that ever touched it, and
returns `fixture_modes` empty where the consumer requires a non-empty
subset of `{name, path}`. The version numbers disagree too — the gate
says 2, the consumer accepts only 1 — but that is the smallest part.

This is not drift between two commits. The version bump in `206811155`
only added compiler bindings; the fields were never there to lose. The
two halves were written to different specifications and have never been
able to run in sequence, which is why the evidence sidecar was empty.

The shape the consumer accepts is not written down anywhere except the
fixture in `tests/cxx/test_parity_manifest.py`, which builds one that
passes. Against what the gate emits:

| record-evidence wants | ctest_gate emits |
|---|---|
| `schema_version: 1` | `2` |
| `shard` | — (`gate_id` is `parity-<shard>`) |
| `gate_id: parity-<shard>` | same, when asked for it |
| `status: passed` | same |
| `ctest_names` | same |
| `cmake_target`, `ctest_label` | — (both are in the registration it captures) |
| `execution_mode: real_tmux`, `real_tmux: true` | — |
| `fixture_modes` ⊆ {name, path}, non-empty | `[]` |
| `result_sha256` | `execution_sha256` |
| `registration_path`, `junit_path` | `artifacts.{registration,junit}` |
| `tmux_binary_sha256`, `tmux_version` | — (`--tmux-bin` is already passed) |
| `evidence_ids` | — |

Most of those the gate could answer from what it already captures. Two
cannot be settled by adding fields, and one of them is a contradiction
rather than a gap.

`evidence_ids` asks the gate to declare which ledger evidence a run
satisfies. A tool that never reads the ledger cannot know that, and the
consumer computes the same list for itself before comparing.

`fixture_modes` is the contradiction. The gate fills it only when the
selector is exactly `{"label": "real-tmux"}` — any other selector
returns an empty list by construction — and that selector runs every
real-tmux test, 57 of them. The consumer requires `fixture_modes` to be
a non-empty subset of `{name, path}` *and* requires `ctest_names` to
have exactly as many entries as the shard has behavior evidence, which
for a shard of three is three. No invocation satisfies both. The two
requirements can only agree for a shard that owns one behavior record
per real-tmux test, which is not what a shard is.

Whoever reconciles these is choosing what a gate run means: the whole
real-tmux suite proving the fixture is sound, or a scoped run proving
one shard. The consumer asks for both at once.

So the reconciliation is a decision about which specification is
canonical, not a patch. The consumer's checks are what tie behavior
evidence to a real-tmux run and to the registration it came from;
relaxing them to fit the producer would remove the guarantee rather than
satisfy it. Extending the producer keeps the guarantee, and means
deciding how it should know each fact — `evidence_ids`, for one, is
something the consumer already computes for itself.

## Still open

Ranked, with the reasoning, in the plan this audit produced. The largest
remaining items are the shape of the API where it freezes at the first tag —
creation verbs that take no tmux flags, entities that are not value types, two
error types that will not compose — and the documentation and examples, which
have to follow those signatures rather than be written twice.
