# Workspace CLI verifiers

End-to-end checks of an installed `tmux-workspace`, on a private tmux socket.
CTest runs both against the built program, as `consumer.workspace.verify.*`,
outside the sanitizer and coverage builds.

| Script | What it checks |
|---|---|
| [`verify_cli.py`](verify_cli.py) | Generated shell completions, `before_script` streaming, cancellation, output limits and closed output, appended loads, the command tree, and a failed write that keeps its destination. It records raw repeated timings of startup, discovery, search and load/capture |
| [`verify_process.py`](verify_process.py) | Process ownership under a real controlling terminal: editor handoff and cancellation, an unbounded writer, and with `--load` and `--logging` the attachment, client selection, retained-change and log-failure cases |

```console
$ uv run scripts/workspace_cli/verify_cli.py \
    --binary build/cxx-dev/apps/workspace/tmux-workspace \
    --iterations 5 \
    --output verify-cli.json
```

```console
$ uv run scripts/workspace_cli/verify_process.py \
    --binary build/cxx-dev/apps/workspace/tmux-workspace \
    --load \
    --output verify-process.json
```

A verifier nobody has watched fail is not a gate. CTest runs each again against
a program that answers nothing, and passes only on the diagnostic it raises.

## Tests

```console
$ uv run --group dev pytest scripts/workspace_cli
```

The tests cover the helpers, and run each verifier against a program that
answers nothing, which must fail.
