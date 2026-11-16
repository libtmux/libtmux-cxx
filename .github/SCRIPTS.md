# Scripts

This policy covers the support scripts in this repository: files under
`scripts/`, CI helpers, generators and checkers. It applies the libtmux
decision in [libtmux/docs#323](https://github.com/libtmux/docs/pull/323).

## Keep a script only for a current use

A script stays when this project runs it today: a CI job, a release step, a
code generator, a documentation check, or a maintainer task named in
[CONTRIBUTING.md](CONTRIBUTING.md).

- Parity with Python libtmux or tmuxp is not a use. Delete comparison
  harnesses, oracles, ledgers and generated fixtures, and the CI steps that
  install them.
- A shell step that creates, prepares or stops tmux for an example or test
  means the library lacks an operation. File an issue for the operation and
  remove the step when it lands.
- Delete scripts nothing calls.
- Tooling several ports share lives once, as a shared workflow or action, not
  as a vendored copy.

## Language

Scripts are Python: the library has no Python dependency, and a small typed
script costs less than a second build system for tooling. Declare dependencies
inline (PEP 723) and run scripts with `uv run`.

## Layout and checks

- A script with tests lives in `scripts/<name>/`, with a `README.md` (what it
  does, who runs it, how) and `tests/`. A short single-purpose script may be
  one file with a usage header.
- Scripts pass `ruff format`, `ruff check`, `ty check` and `pytest`, with
  `pytest-asyncio` for async code and doctests collected, in CI.
- Use current language and tool idioms; no shims below the project's floor.
- Before a new or rewritten script merges, a reviewer who did not write it
  checks it against [WRITING.md](WRITING.md) and
  [no-ai-slop](https://github.com/petergyang/no-ai-slop).

## What stays

- GitHub Actions `run:` lines that call one command.
- Test data that is not a comparison, such as workspace files a loader reads.

## Tmux stand-ins

A test that needs a program in place of tmux copies
[`tests/support/tmux_standin.cpp`](../tests/support/tmux_standin.cpp), a C++
helper with a mode for each behaviour the tests use, and writes its settings
beside the copy with `tests/support/standin.hpp`. No `#!/bin/sh` program stands
in for tmux.

Start cost decides the language. On the maintainer's x86-64 WSL2 host with tmux
3.8, `hyperfine -N --warmup 3 --runs 30` measured one `-V` call, with each
stand-in running the real tmux in its place, at:

| Program | Mean per call |
| --- | --- |
| tmux alone | 1.7 ms |
| the C++ helper, logging the call | 1.7 ms |
| `sh` exec shim | 2.3 ms |
| Python stand-in (`#!/usr/bin/env python3`) | 18.4 ms |

## Exceptions

- `installed_probe.py workspace-completion` runs six lines of bash, because the
  thing under test is the bash completion script `tmux-workspace` prints.
- Integration tests start panes with `sh -c 'sleep 60'` and similar commands.
  They run inside the pane under test; they do not stand in for tmux.
- `release.yml` runs only for a pushed tag. Its logic lives in
  [`scripts/release/`](../scripts/release/README.md) and
  [`scripts/vcpkg/windows_port_gate.py`](../scripts/vcpkg/windows_port_gate.py),
  which their tests exercise on Linux with temporary Git repositories and a
  scripted vcpkg. The workflow itself first runs against a real tag.
- The psmux scripts under [`scripts/psmux/`](../scripts/psmux/README.md) run
  in CTest on a Windows build with psmux installed. No CI lane starts psmux. A
  maintainer runs them with the commands in that README against a psmux build.

