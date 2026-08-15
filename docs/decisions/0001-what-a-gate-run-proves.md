# What a gate run proves

Proposed. Blocks classification of the parity ledger beyond the three
entries recorded so far.

## The contradiction

`record-evidence` cannot accept any record `ctest_gate` can produce, and
the reason is not a missing field.

The gate reports `fixture_modes` only when its selector is exactly
`{"label": "real-tmux"}`; every other selector returns an empty list by
construction. That selector runs all 57 real-tmux tests.

The consumer requires, of one record, both:

- `fixture_modes` — a non-empty subset of `{name, path}`, and
- `len(ctest_names) == len(behavior_ids)`, the shard's behavior evidence.

A shard of three behavior records needs three names. A run that reports
fixture modes has 57. The requirements agree only for a shard owning one
behavior record per real-tmux test, which is not what a shard is.

## What each requirement is for

They are two different assertions that ended up in one record.

`fixture_modes` asserts that the RAII fixture works over both a socket
name and a socket path. That is a property of the fixture, true or false
for the whole suite, and unrelated to which shard is being classified.

`ctest_names` matching the shard's behavior evidence asserts that this
gate ran the tests the evidence cites, and not some other tests. That is
a property of one scoped run.

Merging them asks a single record to be both whole-suite and scoped.

## Proposal

Record them separately.

A per-shard gate record stays scoped: its `ctest_names` are the shard's
behavior tests, and it carries `ctest_label: real-tmux`, which is what
proves the evidence came from a real tmux rather than a stub. Drop
`fixture_modes` from what the consumer demands of it.

Fixture soundness keeps being proven where it already is — the two
closed fixture tests run in every lane on every change, and a regression
there fails the build long before anything reaches the ledger.

The consumer also asks the gate for `shard`, `cmake_target`,
`ctest_label`, `execution_mode`, `real_tmux`, `result_sha256`,
`evidence_ids`, `registration_path`, `junit_path`, `tmux_binary_sha256`
and `tmux_version`. All but one are answerable from what the gate
already captures or is already passed. The exception is `evidence_ids`:
it asks the gate to declare which ledger evidence a run satisfies, which
a tool that never reads the ledger cannot know and the consumer already
computes for itself before comparing. Drop it.

## A better option, found while implementing the one above

Dropping `fixture_modes` from the per-shard record gives up an assertion.
There is a resolution that keeps both.

Let a gate run cover the shard's behavior tests *and* the two closed
fixture tests. Then `fixture_modes` is populated, because the fixture
tests are in the selection, and the shard's tests are all present too.
What has to change is only how the consumer compares: today it requires
`ctest_names` to have exactly as many entries as the shard has behavior
records, and to equal them in order. It would instead require every
behavior record's test to appear among the names.

That keeps the fixture claim, keeps the binding from evidence to test,
and asks the gate for nothing it cannot answer. It is a smaller change
than the one proposed above and gives up nothing, so it is the
preferred resolution.

It was found while implementing the first proposal, which is the reason
this record now recommends against implementing it as written.

## Why this is not a weakening

Nothing here stops being checked. The evidence still has to name a real
test, that test still has to have run under a real tmux, and the
registration and JUnit artifacts are still hash-bound to the record. The
change is where the fixture claim is made, not whether it is made.

The alternative — making every shard's evidence come from a whole-suite
run — would bind 57 test names to three entries and make the ledger say
less about each one, not more.

## A second defect the attempt uncovered

`gtest_discover_tests(... PROPERTIES LABELS "a;b")` applies only the
first label. CMake expands the semicolons before the generated
discovery file is written, so `LABELS "libtmux;real-tmux"` reaches
`set_tests_properties` as `LABELS libtmux real-tmux` and the second name
becomes a stray property. Escaping it as `\;` or `\\;` does not
survive generation.

The consequence is not confined to the ledger: `ctest -L real-tmux`
selects 56 tests and none of the discovered ones, so the whole
control-mode integration suite is absent from any run selected that way.
The tests still run in an unfiltered suite, which is why this has gone
unnoticed.

Resolved by giving discovered tests one label rather than a list. A
single unquoted value has nothing to flatten, so it survives. The
organizational names (`libtmux`, `control`) are dropped for those
targets, which nothing selected on; `real-tmux`, which the evidence gate
selects on, is the one kept.

`ctest -L real-tmux` went from 56 tests to 73, and a whole-label gate
run now reports `fixture_modes: ['name', 'path']` over all 73.

## Status

Not implemented, and the proposal above is superseded by the option that
keeps both assertions. It changes what a verified row means, so it wants
a decision before code.

Implementing it was attempted and reverted. Dropping the two
requirements makes five tests in `test_parity_manifest.py` fail, because
those tests are the contract for the checks being dropped. Rewriting
them is the same decision as changing the contract, and should be taken
deliberately rather than as a consequence of making a pipeline run.
