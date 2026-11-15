#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
r"""Run a Windows example, or the MCP server, against a psmux session it owns.

The runner gives the program a high-entropy ``-L`` namespace, an empty
configuration file created for this run, and no inherited tmux or psmux routing
variables. It creates the one session the program needs, runs the program, then
kills only that exact session, waits for the session's registry files to
disappear and removes the configuration file. If a step fails, the error names
the socket, session and config path to recover.

It never runs psmux session cleanup before a launch attempt, never runs
``kill-server`` and never deletes the ``.psmux`` directory.

    python scripts/psmux/psmux_example.py --example PATH
    python scripts/psmux/psmux_example.py --mcp-server PATH

The example receives the namespace and the session name as arguments. The MCP
server receives ``--socket-name`` and keeps stdin and stdout as its transport.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import sys
import tempfile
import uuid
from collections.abc import Mapping, Sequence

import psmux_fixture as fixture

SESSION = "example"


def new_namespace() -> str:
    """Return a namespace no other run shares."""
    return f"libtmux-cxx-example-{os.getpid()}-{uuid.uuid4().hex}"


def config_for(namespace: str, config_path: str | None) -> pathlib.Path:
    """Return where the isolated configuration lives."""
    if config_path is None:
        return pathlib.Path(tempfile.gettempdir()) / f"{namespace}.conf"
    if not config_path.strip() or not pathlib.Path(config_path).is_absolute():
        message = "ConfigPath must be a non-empty absolute path"
        raise fixture.PsmuxError(message)
    return pathlib.Path(config_path)


def command_for(
    executable: str, namespace: str, *, mcp: bool, session: str = SESSION
) -> list[str]:
    """Return how the program is started against the fixture.

    Examples
    --------
    >>> command_for("example.exe", "ns", mcp=False)
    ['example.exe', 'ns', 'example']
    >>> command_for("server.exe", "ns", mcp=True)
    ['server.exe', '--socket-name', 'ns']
    """
    if mcp:
        return [executable, "--socket-name", namespace]
    return [executable, namespace, session]


class Progress:
    """How far a run got, which decides what its cleanup may touch."""

    def __init__(self) -> None:
        """Start with nothing created."""
        self.created = False
        self.attempted = False


def _host(
    command: Sequence[str],
    *,
    tmux: str,
    namespace: str,
    config: pathlib.Path,
    registry: pathlib.Path,
    environment: Mapping[str, str],
    progress: Progress,
) -> int:
    """Create the config and the session, run ``command`` and return its status."""
    with config.open("x"):
        progress.created = True
    if fixture.registry_entries(registry, f"{namespace}__{SESSION}.*"):
        message = "psmux namespace already has registry state"
        raise fixture.PsmuxError(message)

    progress.attempted = True
    made = subprocess.run(
        [tmux, "-u", "-L", namespace, "new-session", "-d", "-s", SESSION],
        env=dict(environment),
        capture_output=True,
        text=True,
        check=False,
    )
    if made.returncode != 0:
        output = "; ".join(
            part for part in (made.stdout.strip(), made.stderr.strip()) if part
        )
        message = f"psmux fixture creation failed: {output}"
        raise fixture.PsmuxError(message)
    return subprocess.run(list(command), env=dict(environment), check=False).returncode


def run(
    executable: str,
    *,
    mcp: bool = False,
    tmux: str = "tmux.exe",
    config_path: str | None = None,
    environ: Mapping[str, str] | None = None,
    namespace: str | None = None,
) -> int:
    """Create the fixture, run ``executable`` against it and clean up.

    Parameters
    ----------
    executable : str
        The example, or with ``mcp`` the MCP server.
    mcp : bool, optional
        Host the MCP server instead of running an example.
    tmux : str, optional
        The psmux executable.
    config_path : str | None, optional
        An absolute path for the configuration file, which must not exist.
    environ : Mapping[str, str] | None, optional
        The environment to start from, defaulting to this process's.
    namespace : str | None, optional
        A fixed namespace, for tests.

    Returns
    -------
    int
        The program's exit status, or 1 when any step or the cleanup failed.
    """
    base_environment = dict(os.environ if environ is None else environ)
    namespace = namespace or new_namespace()
    base = f"{namespace}__{SESSION}"
    try:
        config = config_for(namespace, config_path)
        registry = (
            pathlib.Path(fixture.resolve_profile_root(base_environment)) / ".psmux"
        )
    except fixture.PsmuxError as error:
        print(error, file=sys.stderr)
        return 1
    environment = fixture.isolated_environment(base_environment, config)

    progress = Progress()
    exit_code = 1
    failure: str | None = None
    try:
        exit_code = _host(
            command_for(executable, namespace, mcp=mcp),
            tmux=tmux,
            namespace=namespace,
            config=config,
            registry=registry,
            environment=environment,
            progress=progress,
        )
    except (fixture.PsmuxError, OSError) as error:
        failure = str(error)
    created, attempted = progress.created, progress.attempted

    proven = not attempted
    if attempted:
        kill_failure = fixture.kill_session(tmux, namespace, SESSION, environment)
        try:
            proven, registry_failure = fixture.prove_registry_clean(
                registry, base, killed=kill_failure is None
            )
        except fixture.PsmuxError as error:
            proven, registry_failure = False, str(error)
        failure = failure or kill_failure or registry_failure
    if proven and created:
        failure = failure or fixture.remove_config(config)
    if failure is not None:
        if attempted or created:
            failure = fixture.recovery(failure, namespace, SESSION, config)
        print(failure, file=sys.stderr)
        return 1
    return exit_code


def main(argv: Sequence[str] | None = None) -> int:
    """Run the command line and return a process status."""
    parser = argparse.ArgumentParser(description=__doc__)
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--example", help="the example program to run")
    target.add_argument("--mcp-server", help="the MCP server to host")
    parser.add_argument("--tmux-executable", default="tmux.exe")
    parser.add_argument("--config-path")
    arguments = parser.parse_args(argv)
    return run(
        arguments.example or arguments.mcp_server,
        mcp=arguments.mcp_server is not None,
        tmux=arguments.tmux_executable,
        config_path=arguments.config_path,
    )


if __name__ == "__main__":
    raise SystemExit(main())
