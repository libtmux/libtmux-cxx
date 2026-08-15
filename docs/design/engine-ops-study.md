# What the engine-ops work taught this library

`libtmux-engine-ops` is the Python experiment in planning tmux work as
operations rather than issuing commands one at a time. Three of its ideas
shaped this library; two did not survive the crossing. The prototype that
carried them here has been deleted, so this records what was kept and why.

## Kept: a status per operation, not per invocation

Engine-ops gives every planned operation its own status, and the vocabulary is
the interesting part: `complete`, `skipped`, and `unknown` are three different
things. `skipped` means an earlier operation failed and this one never ran.
`unknown` means it may have run and the result was lost — a timeout, a dropped
connection.

That distinction is load-bearing because the caller's next move differs. A
skipped operation can be retried freely. An unknown one cannot: retrying may
repeat something that already happened.

It survives here as `Attribution` on control-mode results, and as the reason
`CommandFailure` separates `dispatched` from its `FailureKind`. A timeout is
reported as dispatched precisely because tmux may already have acted.

## Kept: one exit status cannot describe a group

tmux runs a command sequence fail-fast and reports one exit status for the
whole thing. Engine-ops treats that as a limit to work around rather than a
result to report.

`CommandBatch` states the same limit rather than hiding it: `run_batch` reports
the group outcome and cannot say which member failed, and its test proves the
consequence — the commands before a failure have already taken effect, so a
failed batch is partially applied, not rolled back. Per-command attribution is
what a control connection buys, which is why both exist.

## Kept: capture bindings must be strict

Engine-ops lets a later operation refer to something an earlier one created.
The rule it arrived at is that only an exact successful result with a
well-formed entity id may bind, and anything else fails before dispatch rather
than sending a command with a hole in it.

`Chain` keeps the discipline without the mechanism. It validates each step as
it is added and stops accumulating at the first failure, so a chain that never
became valid never reaches tmux. It does not yet support forward references
between steps; when it does, this is the rule to implement.

## Not carried: the planner

Engine-ops resolves dependencies between operations and schedules them. That
belongs to a workspace tool, not to a tmux binding: this library's job is to
make each operation correct and composable, and `WorkspaceBuilder` shows the
scheduling a caller can build on top.

## Not carried: capability adaptation

Engine-ops adapts to tmux version differences by silently changing the command
it emits, for example dropping a flag an older tmux lacks. That trades a clear
error for a quiet behaviour change. Here, `Version` orders releases and
`is_supported` gates the range, and a caller who needs a version-dependent flag
decides for themselves rather than discovering the library chose.
