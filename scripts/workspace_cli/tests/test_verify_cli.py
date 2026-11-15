"""The CLI verifier measures honestly and refuses a program that answers nothing."""

from __future__ import annotations

import pathlib
import shutil
import sys

import pytest
import verify_cli


def program(directory: pathlib.Path, body: str) -> pathlib.Path:
    """Write an executable Python program with `body` as its source."""
    path = directory / "program"
    path.write_text(f"#!{sys.executable}\n{body}\n")
    path.chmod(0o755)
    return path


def test_spread_keeps_the_raw_samples_and_reports_their_range() -> None:
    """Every sample stays, with the median and the extremes beside it."""
    report = verify_cli.spread([4.0, 1.0, 3.0, 2.0, 100.0])
    assert report["samples_ms"] == [4.0, 1.0, 3.0, 2.0, 100.0]
    assert report["median_ms"] == 3.0
    assert (report["min_ms"], report["max_ms"]) == (1.0, 100.0)
    assert report["p95_ms"] == 100.0


def test_run_returns_the_output_and_the_elapsed_time(tmp_path: pathlib.Path) -> None:
    """A passing command's output comes back with a non-negative duration."""
    command = [sys.executable, "-c", "print('ok')"]
    result, elapsed = verify_cli.run(command, {}, tmp_path)
    assert result.stdout.strip() == b"ok"
    assert elapsed >= 0


def test_run_names_the_command_and_its_stderr_when_it_fails(
    tmp_path: pathlib.Path,
) -> None:
    """A nonzero exit raises with the first two words and what was printed."""
    command = [
        sys.executable,
        "-c",
        "import sys; sys.stderr.write('boom'); sys.exit(3)",
    ]
    with pytest.raises(RuntimeError, match=r"-c failed: b'boom'"):
        verify_cli.run(command, {}, tmp_path)


@pytest.mark.skipif(shutil.which("bash") is None, reason="needs a bash to source")
def test_a_program_that_answers_nothing_is_refused(tmp_path: pathlib.Path) -> None:
    """The first check, the generated completion, fails on a program that exits 1."""
    unanswering = program(tmp_path, "raise SystemExit(1)")
    with pytest.raises(RuntimeError, match="--generate-completion failed"):
        verify_cli.main(
            [
                "--binary",
                str(unanswering),
                "--iterations",
                "1",
                "--output",
                str(tmp_path / "report.json"),
            ]
        )
    assert not (tmp_path / "report.json").exists()


def test_a_zero_iteration_count_is_refused(tmp_path: pathlib.Path) -> None:
    """A report with no samples would claim a spread of nothing."""
    with pytest.raises(SystemExit):
        verify_cli.main(
            [
                "--binary",
                str(program(tmp_path, "pass")),
                "--iterations",
                "0",
                "--output",
                str(tmp_path / "report.json"),
            ]
        )
