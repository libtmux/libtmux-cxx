"""The process verifier's helpers, and its refusal of a program that does nothing."""

from __future__ import annotations

import pathlib
import subprocess
import sys
from collections.abc import Callable

import pytest
import verify_process


def query_for(version: str) -> Callable[..., str]:
    """Return a stand-in for the tmux query that answers `version`."""

    def query(*arguments: str) -> str:
        assert arguments == ("display-message", "-p", "#{version}")
        return version

    return query


@pytest.mark.parametrize(
    ("version", "expected"),
    [
        ("3.3a", True),
        ("3.7c", True),
        ("3.8", False),
        ("3.10", False),
        ("4.0", False),
        ("next-3.8", False),
        ("next-3.7", True),
        ("master", True),
    ],
)
def test_the_active_pane_flag_goes_with_tmux_3_8(version: str, expected: bool) -> None:
    """From 3.8 on, a development tree included, nothing can reach the refusal."""
    assert verify_process.has_active_pane_flag(query_for(version)) is expected


def test_a_socket_inside_sun_path_is_named_under_its_root(
    tmp_path: pathlib.Path,
) -> None:
    """The socket sits beside the fixture's other files."""
    assert verify_process.socket_path(tmp_path) == str(tmp_path / "tmux.sock")


def test_a_socket_sun_path_cannot_hold_is_refused(tmp_path: pathlib.Path) -> None:
    """A path past the sockaddr limit would fail obscurely inside tmux."""
    long = tmp_path / ("d" * verify_process.SUN_PATH)
    with pytest.raises(RuntimeError, match="exceeds sun_path"):
        verify_process.socket_path(long)


def test_the_log_limit_is_set_inside_the_process_that_runs_the_cli() -> None:
    """The wrapper caps file size, then execs the real program."""
    command = verify_process.with_log_limit(
        [
            sys.executable,
            "-c",
            "import resource; print(resource.getrlimit(resource.RLIMIT_FSIZE))",
        ]
    )
    done = subprocess.run(command, capture_output=True, text=True, check=True)
    assert done.stdout.strip() == "(2048, 2048)"


def test_a_program_that_does_nothing_fails_the_first_terminal_check(
    tmp_path: pathlib.Path,
) -> None:
    """The editor handoff cannot be observed from a program that exits 1."""
    unanswering = tmp_path / "unanswering"
    unanswering.write_text(f"#!{sys.executable}\nraise SystemExit(1)\n")
    unanswering.chmod(0o755)
    with pytest.raises(AssertionError):
        verify_process.main(["--binary", str(unanswering)])
