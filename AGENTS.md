# Agent instructions

Follow the existing project conventions and keep changes narrowly scoped to
what was asked for.

## Never touch the default tmux server

Several libtmux ports run their suites on this kind of machine at once, and a
tmux server is shared state keyed only by its socket. Use `ScopedTmuxServer` in
tests and a private `TMUX_TMPDIR` for one-off probes; never run a bare `tmux`
against the default server, where somebody's real session lives. The recipe is
in [`.github/CONTRIBUTING.md`](.github/CONTRIBUTING.md).

## Additional guidance

This file routes; it does not restate. Read the policy that governs the change
being made:

- Documentation and user-facing prose — `README.md`, `CHANGELOG.md`, release
  notes, commit messages, CLI and help text, API documentation, and source
  comments: [`.github/WRITING.md`](.github/WRITING.md)
- Contribution workflow, building, testing, and pull requests:
  [`.github/CONTRIBUTING.md`](.github/CONTRIBUTING.md)
