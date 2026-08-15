# Control-mode writer residency

## What was observed

`graft.control.integration.ControlModeConnection.WriterWaitHonorsDeadlineWithoutPoisoningOwner`
fails roughly once in five to eight runs, in every preset, on an otherwise idle
machine. When it fails, all three message assertions report the same string:

```text
control request deadline expired after dispatch
```

The waiter expects `control writer acquisition deadline expired`. Getting
`after dispatch` instead means the waiter acquired the writer and sent its
command, so the owner was not holding the writer at that moment. The resulting
connection error then propagates to the owner's result and to shutdown, which
is why one race produces three failures.

## What it means

This is a test-design defect, not a connection defect. In both observed
outcomes the connection reported a coherent error that matches what actually
happened; the test asserts one exact message that is only correct while a
timing precondition holds, and it does not establish that precondition
deterministically.

The test arranges residency by stopping the client with `SIGSTOP` and writing
four megabytes, then sleeping one second before the waiter arrives. That is an
assumption about when the owner's write blocks, not a guarantee. Whatever
makes the owner release the writer early — the write completing into buffers,
the write failing, or the owner thread not yet having reached it — leaves the
waiter free to dispatch, and the test then measures a different scenario from
the one it names.

## Why it is recorded rather than fixed here

The graft is spike evidence whose gate digest is bound into the transport
decision. Editing the test changes that gate, which requires re-running both
hard gates and re-freezing the decision. That sequence belongs with the
correction below, not ahead of it.

## Correction owed to the decision

`docs/bakeoffs/transport/decision.json` carries a limitation named
`control_mode_release_deadline` whose rationale says the behaviour is scoped to
release optimization. That is wrong on both scope and cause: the failure occurs
in every preset, and its cause is writer residency in the test rather than
build configuration. The rationale needs replacing with the finding above, and
because the decision is digest-bound that means re-running the gates and
regenerating the decision and scorecard.

Two earlier characterisations of this failure were also wrong and are corrected
by the evidence above: it is neither release-only nor deterministic.

## Resolution

The test now reports whether residency actually held instead of assuming it.
An attempt that finds the waiter dispatched — the interleaving described above
— is retried rather than asserted against, and the contract assertions run only
against a run that reached the scenario. Five attempts that never reach it fail
the test with that reason, so the case cannot pass vacuously.

The assertion the test exists for is stated directly: the owner is ended by
shutdown, never by the waiter's timeout. Eleven consecutive runs passed where
the previous form failed roughly one in six.

The correction owed to `decision.json` stands: its recorded cause and scope
were both wrong, and the replacement text is in `transport-decision-audit.md`.
