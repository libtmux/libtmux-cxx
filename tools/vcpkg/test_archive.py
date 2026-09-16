"""What a release tarball carries.

`vcpkg_from_github` downloads GitHub's source tarball, which `git archive`
builds. Whatever is in it is what a consumer vendors, so this asserts both
halves: the research record is out, and everything needed to build and read
the library is still in.
"""

from __future__ import annotations

import io
import pathlib
import subprocess
import tarfile
import unittest

EXCLUDED = ("tools/", ".github/")
REQUIRED = (
    "CMakeLists.txt",
    "LICENSE",
    "VERSION",
    "README.md",
    "include/libtmux/libtmux.hpp",
    "src/server.cpp",
    "cmake/libtmux.pc.in",
    "docs/api.md",
    "ports/libtmux/portfile.cmake",
)


def archived_paths() -> set[str]:
    """Every path `git archive HEAD` writes, as the tarball spells them."""
    root = pathlib.Path(__file__).resolve().parents[2]
    archive = subprocess.run(
        ["git", "archive", "HEAD"], cwd=root, capture_output=True, check=True
    ).stdout
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        return {member.name for member in tar.getmembers()}


class ArchiveContents(unittest.TestCase):
    """What the release tarball carries, and what it does not."""

    def test_the_research_record_does_not_ship(self) -> None:
        """The parity ledger and the CI definitions are not part of the library."""
        paths = archived_paths()
        for prefix in EXCLUDED:
            carried = sorted(path for path in paths if path.startswith(prefix))
            self.assertEqual(
                carried,
                [],
                f"{prefix} is vendored with the library; it is not part of it",
            )

    def test_the_library_still_ships(self) -> None:
        """Excluding the record must not take the library with it."""
        paths = archived_paths()
        for required in REQUIRED:
            self.assertIn(
                required, paths, f"{required} is needed to build or read the library"
            )


if __name__ == "__main__":
    unittest.main()
