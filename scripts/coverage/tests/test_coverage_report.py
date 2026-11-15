"""Keep the coverage report to the binaries the run instrumented."""

import contextlib
import io
import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import coverage_report
from coverage_report import instrumented_binaries


class InstrumentedBinaries(unittest.TestCase):
    """Name only what `llvm-cov` can read as an object."""

    def make(self, root: Path, name: str, contents: bytes) -> Path:
        """Write an executable file below the build tree."""
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
        return path

    def test_a_script_the_sandbox_tests_put_ahead_of_tmux_is_not_an_object(self):
        """Leave out the stand-in `tmux`, which is a shell script."""
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            test = self.make(build, "tests/libtmux_example_test", b"\x7fELF")
            self.make(build, "sandbox-wrapper/tmux", b"#!/bin/sh\nexec tmux\n")
            self.assertEqual(instrumented_binaries(build), [test])

    def test_ordinary_binaries_are_all_named(self):
        """Keep every executable the run produced, in a stable order."""
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            second = self.make(build, "apps/mcp/server", b"\x7fELF")
            first = self.make(build, "apps/mcp/probe", b"\x7fELF")
            self.assertEqual(instrumented_binaries(build), [first, second])


TOTAL = "TOTAL  100  20  80.00%  50  10  80.00%  400  {missed}  {covered}%  0  0  -"


class Floor(unittest.TestCase):
    """Fail the job when line coverage collapses, and only then."""

    def run_report(self, covered: float, floor: float) -> tuple[int, str]:
        """Run `main` over a build whose `llvm-cov` reports `covered` percent."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bin_dir = root / "bin"
            bin_dir.mkdir()
            total = TOTAL.format(
                missed=int(400 * (100 - covered) / 100), covered=covered
            )
            fakes = {
                "llvm-profdata-18": "",
                "llvm-cov-18": f"print('Filename Regions\\n{total}')",
            }
            for name, body in fakes.items():
                tool = bin_dir / name
                tool.write_text(f"#!{sys.executable}\n{body}\n")
                tool.chmod(0o755)
            build = root / "build"
            (build / "profiles").mkdir(parents=True)
            binary = build / "tests" / "suite"
            binary.parent.mkdir()
            binary.write_bytes(b"\x7fELF")
            binary.chmod(0o755)
            os.utime(binary, (1, 1))
            (build / "profiles" / "a.profraw").write_bytes(b"x")
            environment = {"PATH": f"{bin_dir}{os.pathsep}{os.environ['PATH']}"}
            stderr = io.StringIO()
            with (
                mock.patch.dict(os.environ, environment),
                contextlib.redirect_stdout(io.StringIO()),
                contextlib.redirect_stderr(stderr),
            ):
                status = coverage_report.main(
                    [
                        "--build",
                        str(build),
                        "--source",
                        str(root),
                        "--floor",
                        str(floor),
                    ]
                )
            return status, stderr.getvalue()

    def test_coverage_at_the_floor_passes(self) -> None:
        """A figure at or over the floor is not a failure."""
        self.assertEqual(self.run_report(covered=70.0, floor=65.0), (0, ""))

    def test_coverage_under_the_floor_fails_and_says_so(self) -> None:
        """A collapse below the floor is reported with both numbers."""
        status, message = self.run_report(covered=60.0, floor=65.0)
        self.assertEqual(status, 1)
        self.assertIn("line coverage 60.00% is below the floor of 65.00%", message)
