# CI plan

Plans which tmux releases CI runs, and checks that the pages and workflows
that mention them agree with one manifest.

[`tmux_matrix.json`](tmux_matrix.json) is the only list of tmux releases. It
names the released versions, the preview refs, the refused releases and the
pinned compiler. Adding a release there is the whole change to CI.

## Plan the matrix

`ci.yml` runs the planner in its `plan` job and feeds the legs to the
`compatibility` and `rejection` jobs. A push or pull request gets the oldest
and newest release; a release and `scope: full` get the whole table. The same
command prints a plan without a runner:

```console
$ uv run scripts/ci_plan/ci_plan.py matrix --event push
```

A dispatch names its own refs:

```console
$ uv run scripts/ci_plan/ci_plan.py matrix --event workflow_dispatch --scope custom --tmux-ref 3.6b,master
```

The `all-checks-passed` job calls `ci_plan.py verify` to compare each job's
result with what the plan expected.

## Check the pages and workflows

[`ci_check.py`](ci_check.py) fails when the compatibility page or CONTRIBUTING
state a range the manifest does not, when a workflow carries a version list of
its own, or when the structure the plan relies on has been undone: an input
spliced into a script, an unpinned tmate step, a job missing from the
aggregate, a raw `apt-get` call, or a shared action that is not pinned to a
full commit.

```console
$ uv run scripts/ci_plan/ci_check.py
```

## Tests

```console
$ uv run --group dev pytest scripts/ci_plan
```

Each rule has a case that breaks a copy of the tree and expects the guard to
notice.
