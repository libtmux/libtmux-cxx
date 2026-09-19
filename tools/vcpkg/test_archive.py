"""What a release tarball carries.

`vcpkg_from_github` downloads GitHub's source tarball, which `git archive`
builds. Whatever is in it is what a consumer vendors, so this asserts both
halves: the research record is out, and everything needed to build and read
the library is still in.
"""

from __future__ import annotations

import io
import pathlib
import re
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


class ConsumerDependencies(unittest.TestCase):
    """What a consumer of the library takes on by consuming it."""

    def test_the_library_does_not_carry_the_mcp_servers_parser(self) -> None:
        """The MCP server needs a JSON parser; the library must not inherit it."""
        root = pathlib.Path(__file__).resolve().parents[2]
        carrying = [
            str(path.relative_to(root))
            for directory in ("include", "src")
            for path in (root / directory).rglob("*")
            if path.is_file()
            and path.suffix in {".hpp", ".cpp", ".txt", ".in"}
            and "nlohmann" in path.read_text(errors="ignore")
        ]
        self.assertEqual(
            carrying,
            [],
            "the library reaches the MCP server's parser; a consumer that never "
            "asked for the server would inherit it",
        )

    def test_the_mcp_server_is_not_built_by_default(self) -> None:
        """A library build does not become a program build without being asked."""
        root = pathlib.Path(__file__).resolve().parents[2]
        declaration = (root / "CMakeLists.txt").read_text()
        # The one line, not the whole file: an assertion that dumps 200 lines
        # of CMake to say one option changed is not a readable failure.
        option = re.search(
            r"^option\(LIBTMUX_BUILD_MCP_SERVER .*?\)$",
            declaration,
            re.MULTILINE | re.DOTALL,
        )
        self.assertIsNotNone(option, "LIBTMUX_BUILD_MCP_SERVER is no longer declared")
        assert option is not None
        self.assertTrue(
            option.group().rstrip().endswith("OFF)"),
            "the MCP server is opt-in; defaulting it on hands every consumer a "
            f"program and its parser: {option.group()}",
        )


if __name__ == "__main__":
    unittest.main()
