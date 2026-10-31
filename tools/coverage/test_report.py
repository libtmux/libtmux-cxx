"""Keep the coverage report to the binaries the run instrumented."""

import stat
import tempfile
import unittest
from pathlib import Path

from tools.coverage.report import instrumented_binaries


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


if __name__ == "__main__":
    unittest.main()
