# Working in `cxx/`

Guidance specific to this port. The repository root `AGENTS.md` still
applies.

## Never share a tmux server with anything else

Several libtmux ports live on this machine at once — other languages, other
checkouts, each running its own test suite. They all talk to tmux, and a
tmux server is shared state keyed only by its socket. Two suites on one
socket kill each other's sessions and produce failures that look like real
bugs in whichever one noticed first.

So every tmux this workspace starts gets a socket of its own, under a
directory named for the workspace:

- **Tests** already do this. `ScopedTmuxServer` makes a private tree with
  `mkdtemp` at `$TMPDIR/libtmux-cxx-test-XXXXXX`, points `TMUX_TMPDIR`
  inside it, and addresses the server with `-L server` or `-S <tree>/socket`
  under that tree. It erases `TMUX` and `TMUX_PANE` from the child
  environment so a suite run from inside tmux cannot reach the outer server.
  Use the fixture; do not start tmux another way in a test.
- **Spikes and one-off probes** must do the same by hand:

```console
$ export TMUX_TMPDIR="$(mktemp -d -p /tmp libtmux-cxx-spike-XXXXXX)"
```

  then `unset TMUX TMUX_PANE`, address the server with `-L` or `-S` under
  that directory, `kill-server` when finished, and remove the tree.

Never run a bare `tmux` command against the default server while working
here. It is somebody's real session, and a stray `kill-server` takes their
work with it.

The socket path has to fit in `sun_path`, so keep the prefix short;
`socket_path_fits` in the fixture reports the case where it does not rather
than letting tmux fail obscurely.

## The parity ledger is read here, not written

The ledger in `tools/parity/data` records what each Python `libtmux`
symbol maps to and the evidence for it. Two halves of the tooling read it,
and only one of them can run in this repository:

- `verify`, `gaps`, `coverage` and `record-evidence` read the recorded
  artifacts and this tree. They work, and the gate below runs them.
- `generate` and `drift` observe the Python library at a pinned commit to
  produce the ledger in the first place. They need that repository's files
  *and its history* — `inputs.json` pins `src/libtmux`, `conftest.py` and
  `pyproject.toml` by git object, none of which exist here.

So the recorded surface is frozen at the commit it was taken from. Adding
a classification means editing `mapping.json` and `evidence.json` and
re-running `sync`, which is what every entry so far has done anyway.
Regenerating from a newer Python release means doing it where that
release lives.

## Before committing

Five build lanes, the format and reference checks, the parity ledger, and
the mutation catalogue:

```console
$ for p in cxx-dev cxx-sanitize cxx-tsan cxx-gcc cxx20; do cmake --preset $p && cmake --build --preset $p && ctest --preset $p --no-tests=error; done
```

```console
$ python3 -m tools.parity verify --manifest tools/parity/data/manifest.json --mode structural --allow-pending
```

```console
$ python3 -m tools.mutate --preset cxx-dev
```

The mutation runner edits sources in place and puts each back as it goes.
Do not commit or build from the tree while it runs: a commit taken during a
run once captured a `load-buffer` without the flag naming its buffer, and
the diff read like ordinary work.

## Comments earn their maintenance cost

A comment ships only if it passes all three gates. Fail any: delete or rewrite.
Borderline: delete — borderline means the information is reconstructible, which
is what makes deletion cheap.

**Loss.** Three years from now, would losing this cost a maintainer real time
rediscovering intent, an invariant, a constraint, or a failure mode the code and
tests do not already make obvious?

**Elite.** Would SQLite, Redis, the Go standard library, or CPython write this
comment, at this length? Those projects state the constraint and stop. They do
not argue with an imagined objector.

**Upkeep.** Will it stay true without maintenance? A comment that hand-syncs a
value the code owns — a count, an offset, a line reference, a duplicated
constant — is false the first time that value moves.

### Ceiling

One or two lines. A comment reaching four is either carrying several facts, in
which case split it, or arguing, in which case cut it to the fact.

Rationale, alternatives weighed, and the story of how the code got here belong
in the commit message: timestamped, attached to the exact diff, and free to
maintain.

A comment often holds both a constraint and the deliberation that found it. Keep
the constraint, cut the deliberation. "Runs at most once per second" survives;
"this is the right trade for now" does not.

### Keep

- Why over how: upstream quirks, protocol and compatibility constraints,
  performance tradeoffs still part of the contract.
- Invariants, preconditions, ordering, lifetime, and concurrency requirements
  that types and tests cannot express.
- Code that looks wrong but is not, so a later cleanup does not reintroduce the
  bug.
- A high-level sketch of an algorithm whose local operations do not reveal the
  whole.

### Delete

- Narration of the next lines; code translated into English.
- Restated names, types, defaults, or control flow.
- Values duplicated from the code and hand-synced.
- Justification, hedging, or apology for a choice.
- Speculation about future requirements.
- History version control already holds, including commented-out code.
- Ticket and issue numbers. They say nothing to a reader without tracker access,
  and they rot when the tracker moves. Unfinished work goes in the tracker, not
  the source.
- Transient observations — "currently", "for now", "the latest release" —
  that go stale with no nearby edit.

### The upkeep gate in practice

It reaches values that track our own code. It does not reach frozen external
facts.

Bad (Delete):

```python
# There are 321 tests to complete for servers.
```

Good (Keep):

```python
# tmux < 3.2 reports the pane ID only after the command completes,
# so this query must stay separate.
```

### Documentation exception

Doctests, minimal usage examples, and param, return, and raises lines on public
API are exempt from the loss gate — they serve the caller, not the maintainer.
They are exempt from nothing else. Ceiling: a good man page entry.

NumPy-style `Parameters`, `Returns`, and `Attributes` sections and executable
doctests fall under this exception — autodoc ships every field whether or not
you describe it, and a doctest that runs is also a test. Public-header doc
comments fall under this exception.

## Git Commit Standards

Format commit messages as:
```
Scope(type[detail]): concise description

why: Explanation of necessity or impact.

what:
- Specific technical changes made
- Focused on a single topic
```

Keep the subject ≤50 chars (excluding any trailing `(#NN)` PR ref); wrap
body lines at ≤72 chars. Separate the `why:` and `what:` blocks with a
blank line.

Common commit types:
- **feat**: New features or enhancements
- **fix**: Bug fixes
- **refactor**: Code restructuring without functional change
- **docs**: Documentation updates
- **chore**: Maintenance (dependencies, tooling, config)
- **test**: Test-related updates
- **style**: Code style and formatting
- **cxx(deps)**: Dependencies
- **cxx(deps[dev])**: Dev Dependencies
- **ai(rules[AGENTS])**: AI rule updates

Example:
```
Pane(feat[send_keys]): Add support for a literal flag

why: Send characters without tmux interpreting them.

what:
- Add a literal field to send_keys_options
- Pass -l when it is set
```

### Release commits

Never create tags. Never push tags. The user handles tagging and tag
pushes (tags trigger the CI publish workflow).

Release commit subjects are plain and short: `Tag v<version>`. Put
the detailed why/what in the commit body. Don't use the
`Scope(type[detail]):` format for releases — don't bury the lede.

For multi-line commits, use heredoc to preserve formatting:
```bash
git commit -m "$(cat <<'EOF'
Scope(feat[detail]): Concise description

why: Explanation of the change.

what:
- First change
- Second change
EOF
)"
```

## Code Blocks

Code blocks are paste-and-run units: pasting one block runs exactly one
intended action. Doctests and other executed examples are exempt — the test
suite runs them, nobody pastes them.

- **One command per block.** Multiple steps may share a block only when
  explicitly chained with `&&`, `;`, or `\` continuations — the chain is
  then one logical command.
- **Explanations go in prose above the block**, never as `#` comments inside it.
- **Command menus are per-command blocks with prose lead-ins**, not tables.
- **Shell commands use the `console` tag with a `$ ` prefix.** This separates
  interactive commands from scripts and enables prompt-aware copy.
- **Split long commands with `\`** — one flag or flag+value pair per indented
  continuation line, positional arguments last.

Good:

Show the last ten commits as a graph:

```console
$ git log \
    --max-count=10 \
    --graph \
    --oneline
```

Bad:

```console
# Show the last ten commits as a graph
$ git log --max-count=10 --graph --oneline
```
