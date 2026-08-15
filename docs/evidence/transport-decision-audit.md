# Transport decision audit

## What the decision describes

`cxx/docs/bakeoffs/transport/decision.json` records a bakeoff performed at
commit `90ffde066`, whose tree is `579a9ae9`. That binding is intact: the tree
recorded in the decision is still the tree of that commit.

It does not describe the current tree, and it should not. The measurements
compare three prototype backends against each other under one compiler and one
workload at one moment. Re-freezing them against today's source would claim
they describe code they never ran on.

## What that changes about verification

`verify_decision` re-derives each measured source digest from the live
worktree. That was correct while the bakeoff was the work in hand: the tree it
verified against was the tree that had just been measured. It is wrong now.
Running it today reports

```text
measured source file digest does not match the live file
```

which is not a tampering signal — it is the tool stating that the library has
been written since. Verification of a historical decision belongs against the
commit it names, which is what the tree check above performs and what a
deletion audit has to preserve once the measured files are gone.

## Correction to a recorded limitation

The decision carries a limitation `control_mode_release_deadline` whose
rationale says the behaviour is scoped to release optimization. That is wrong
on scope and on cause. The failure occurs in every preset, and its cause is
writer residency in the test rather than build configuration:
`WriterWaitHonorsDeadlineWithoutPoisoningOwner` assumes the owner still holds
the writer when the waiter arrives, and arranges that by stopping the client
and writing four megabytes. When the owner releases early the waiter dispatches
and times out after dispatch, poisoning the connection and failing all three of
the test's message assertions at once. The full observation is in
`control-mode-writer-residency.md`.

The test has since moved. It is no longer graft-bound; it runs as
`libtmux.control.integration.ControlModeConnection.WriterWaitHonorsDeadlineWithoutPoisoningOwner`
against the shipped connection, so the flake now surfaces in the library suite
where it belongs.

## Why the rationale is corrected here rather than in the decision

Editing `decision.json` changes its core digest, which the review closure and
scorecard are bound to. Regenerating those means re-running both hard gates and
the measurement — against a tree whose contenders the library has already
superseded. That would produce a decision documenting a comparison that no
longer exists, to fix one sentence.

The correction is therefore recorded against the decision rather than inside
it, and this file is the audit a reader is meant to reach from it. If the
decision is ever regenerated for another reason, the rationale above is the
text that belongs in it.

## Deletion

`cxx/spikes/` and `cxx/tests/contracts/` are deleted. That removes the three
transport contenders, the engine-ops and control-mode grafts, the query
prototype, and the contract suite that exercised the contenders against each
other.

The measured files the decision names are therefore no longer in the worktree.
Verification of the decision is against commit `90ffde066`, whose tree
`579a9ae9` still contains them; the tree check recorded above is what replaces
live-file digest re-derivation.

What the contract suite proved lives on where callers can reach it:
`libtmux.server_contract` pins argv reaching tmux without a shell, byte
preservation, the failure taxonomy, timeout behaviour, and descriptor
stability; `libtmux.control.*` pins parser framing and the connection's
shutdown, ownership, and attribution behaviour.

The deletion found one defect the internal suite could not. `library_version`
was declared inside the ABI inline namespace and defined in the enclosing one,
which makes a different symbol. Class members were unaffected because lookup
finds their class through the inline namespace; a free function is not. No
internal test calls it, so only an outside consumer linking the installed
package could fail — which is what happened, before any release did.
