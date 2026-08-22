# Agent instructions

Follow the existing project conventions and keep changes narrowly scoped to
what was asked for.

## Never touch the default tmux server

Several libtmux ports run their suites on this kind of machine at once, and a
tmux server is shared state keyed only by its socket. Use `ScopedTmuxServer` in
tests and a private `TMUX_TMPDIR` for one-off probes; never run a bare `tmux`
against the default server, where somebody's real session lives. The recipe is
in [`.github/CONTRIBUTING.md`](.github/CONTRIBUTING.md).

## Change discipline

- Make the smallest coherent change that solves the verified problem, and keep
  unrelated cleanup out of it.
- Reuse an existing file, helper, type or test before adding a new one.
- Keep a new declaration out of `include/libtmux/` until a caller outside the
  library needs it. If it is there, it is the contract.
- Add a file only for a durable boundary — a distinct responsibility,
  independent reuse, or splitting an oversized one — not for a single-use
  helper.
- A passing gate is evidence only once it has been shown able to fail. Pair a
  new test with a deliberate break that proves it bites.

## Additional guidance

This file routes; it does not restate. Read the policy that governs the change
being made:

- Documentation and user-facing prose — `README.md`, `CHANGELOG.md`, release
  notes, commit messages, CLI and help text, API documentation, and source
  comments: [`.github/WRITING.md`](.github/WRITING.md)
- Contribution workflow, building, testing, and pull requests:
  [`.github/CONTRIBUTING.md`](.github/CONTRIBUTING.md)

Each is the single home for its subject. Where a rule looks stated twice, the
file named above governs.
