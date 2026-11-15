"""Hold the include budget to a ratio, and fail when the ratio is over."""

from __future__ import annotations

import contextlib
import io
import pathlib
import sys

import include_cost
import pytest

FAKE_COMPILER = """#!{python}
import sys
unit = open(sys.argv[-1]).read()
sys.stdout.write("x" * ({umbrella} if "libtmux" in unit else {baseline}))
"""


def fake_compiler(
    directory: pathlib.Path, *, baseline: int = 1000, umbrella: int = 3000
) -> pathlib.Path:
    """Write a compiler that preprocesses a unit to a size set by its content."""
    compiler = directory / "fake-c++"
    compiler.write_text(
        FAKE_COMPILER.format(
            python=sys.executable, baseline=baseline, umbrella=umbrella
        )
    )
    compiler.chmod(0o755)
    return compiler


def run(compiler: pathlib.Path, *extra: str) -> tuple[int, str, str]:
    """Run `main` with the fake compiler and return status, stdout and stderr."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        status = include_cost.main(["--compiler", str(compiler), *extra])
    return status, out.getvalue(), err.getvalue()


def test_a_header_inside_the_budget_passes_under_check(tmp_path: pathlib.Path) -> None:
    """A ratio under the budget reports it and exits zero."""
    status, out, _ = run(fake_compiler(tmp_path), "--check")
    assert status == 0
    assert "3.00x a <string> unit" in out


def test_a_header_over_the_budget_fails_under_check(tmp_path: pathlib.Path) -> None:
    """A ratio over the budget is the failure this check exists to raise."""
    compiler = fake_compiler(tmp_path, umbrella=4500)
    status, _, err = run(compiler, "--check")
    assert status == 1
    assert "over budget: 4.50x exceeds 4.00x" in err


def test_the_same_ratio_is_only_reported_without_check(tmp_path: pathlib.Path) -> None:
    """Without `--check` the ratio is information, not a gate."""
    compiler = fake_compiler(tmp_path, umbrella=4500)
    status, out, _ = run(compiler)
    assert status == 0
    assert "4.50x" in out


def test_a_budget_on_the_command_line_replaces_the_default(
    tmp_path: pathlib.Path,
) -> None:
    """`--budget` moves the line the ratio is held to."""
    compiler = fake_compiler(tmp_path, umbrella=4500)
    assert run(compiler, "--check", "--budget", "5")[0] == 0


def test_a_compiler_that_cannot_preprocess_stops_the_run(
    tmp_path: pathlib.Path,
) -> None:
    """The compiler's own diagnostic is what the failure says."""
    compiler = tmp_path / "broken-c++"
    compiler.write_text(
        f"#!{sys.executable}\nimport sys\nsys.stderr.write('no such header')\n"
        "sys.exit(1)\n"
    )
    compiler.chmod(0o755)
    with pytest.raises(SystemExit, match="no such header"):
        run(compiler, "--check")


def test_a_baseline_that_preprocesses_to_nothing_is_refused(
    tmp_path: pathlib.Path,
) -> None:
    """A ratio against zero bytes would be meaningless."""
    with pytest.raises(SystemExit, match="preprocessed to nothing"):
        run(fake_compiler(tmp_path, baseline=0), "--check")
