"""Regression tests for the README-quoting fixer."""

from __future__ import annotations

import pathlib
import tempfile
import unittest

from tools.docs import check_readme


class MatchingLineTest(unittest.TestCase):
    """The line `_fix` correlates a block by must survive a Given line."""

    def test_skips_a_leading_given_line(self) -> None:
        """A region's assumption is not the text a block is matched on."""
        body = "// Given: const libtmux::Server& server\nDoes the thing.\nact();\n"

        self.assertEqual(check_readme._matching_line(body), "Does the thing.")

    def test_a_body_with_no_given_line_is_unaffected(self) -> None:
        """A block quoted before this convention existed still matches."""
        body = "Does the thing.\nact();\n"

        self.assertEqual(check_readme._matching_line(body), "Does the thing.")


class FixTest(unittest.TestCase):
    """`--fix` must not collapse two regions that share a Given line."""

    def test_shared_given_line_does_not_collide(self) -> None:
        """Two regions given the same type differ by their second line only.

        Matching on the literal first line - what this checked before -
        would key both regions on "// Given: const libtmux::Server& server"
        and the dict comprehension building `by_first` would keep only the
        second one, silently mismatching (or, as observed live, leaving
        every block unmatched once real regions started declaring a Given).
        """
        available = {
            "connect": (
                "// Given: const libtmux::Server& server\n"
                "Connects to it.\n"
                "server.sessions();\n"
            ),
            "escape": (
                "// Given: const libtmux::Server& server\n"
                "Escapes to it.\n"
                "server.run({});\n"
            ),
        }
        # The blocks as they exist in a README predating the Given
        # convention: same first line the region now starts with (its
        # description), no Given line yet.
        readme_text = (
            "```cpp\n"
            "Connects to it.\n"
            "stale_connect();\n"
            "```\n"
            "\n"
            "```cpp\n"
            "Escapes to it.\n"
            "stale_escape();\n"
            "```\n"
        )

        with tempfile.TemporaryDirectory() as directory:
            readme = pathlib.Path(directory) / "README.md"
            readme.write_text(readme_text, encoding="utf-8")

            status = check_readme._fix(readme, pathlib.Path("example.cpp"), available)

            self.assertEqual(status, 0)
            updated = readme.read_text(encoding="utf-8")
            self.assertIn(available["connect"], updated)
            self.assertIn(available["escape"], updated)


class CheckTest(unittest.TestCase):
    """`check` holds every README to the regions, and the regions to the READMEs."""

    available = {  # noqa: RUF012 - read, never mutated
        "first": "Does the first thing.\nact_first();",
        "second": "Does the second thing.\nact_second();",
    }

    def readmes(self, directory: str, *texts: str) -> list[pathlib.Path]:
        """Write each text as a README of its own, in order."""
        paths = []
        for index, text in enumerate(texts):
            path = pathlib.Path(directory) / f"README{index}.md"
            path.write_text(text, encoding="utf-8")
            paths.append(path)
        return paths

    def test_a_region_quoted_by_either_readme_is_not_stale(self) -> None:
        """The package README may quote what the top-level one does not."""
        with tempfile.TemporaryDirectory() as directory:
            paths = self.readmes(
                directory,
                "```cpp\nDoes the first thing.\nact_first();\n```\n",
                "```cpp\nDoes the second thing.\nact_second();\n```\n",
            )

            problems, blocks = check_readme.check(
                paths, pathlib.Path("example.cpp"), self.available, frozenset()
            )

        self.assertEqual(problems, [])
        self.assertEqual(blocks, 2)

    def test_a_region_no_readme_quotes_is_reported(self) -> None:
        """Compiled code that nobody sees still fails the check."""
        with tempfile.TemporaryDirectory() as directory:
            paths = self.readmes(
                directory, "```cpp\nDoes the first thing.\nact_first();\n```\n"
            )

            problems, _ = check_readme.check(
                paths, pathlib.Path("example.cpp"), self.available, frozenset()
            )

        self.assertEqual(len(problems), 1)
        self.assertIn("region 'second'", problems[0])

    def test_a_drifted_block_in_the_second_readme_is_reported(self) -> None:
        """A block that matches no region fails, naming the README that has it."""
        with tempfile.TemporaryDirectory() as directory:
            paths = self.readmes(
                directory,
                "```cpp\nDoes the first thing.\nact_first();\n```\n"
                "```cpp\nDoes the second thing.\nact_second();\n```\n",
                "```cpp\nDoes the second thing.\nact_changed();\n```\n",
            )

            problems, _ = check_readme.check(
                paths, pathlib.Path("example.cpp"), self.available, frozenset()
            )

        self.assertEqual(len(problems), 1)
        self.assertIn("README1.md: C++ block 1", problems[0])
        self.assertIn("closest: 'second'", problems[0])

    def test_an_include_block_must_name_a_header_the_example_includes(self) -> None:
        """A block of `#include` lines has no region; the example vouches for it."""
        shown = "```cpp\n#include <libtmux/libtmux.hpp>\n```\n"
        misspelt = "```cpp\n#include <libtmux/libtmuxx.hpp>\n```\n"
        includes = frozenset({"#include <libtmux/libtmux.hpp>"})
        available = {"first": "Does the first thing.\nact_first();"}
        quoting = "```cpp\nDoes the first thing.\nact_first();\n```\n"

        with tempfile.TemporaryDirectory() as clean_directory:
            clean = self.readmes(clean_directory, shown + quoting)
            clean_problems, _ = check_readme.check(
                clean, pathlib.Path("example.cpp"), available, includes
            )
        with tempfile.TemporaryDirectory() as broken_directory:
            broken = self.readmes(broken_directory, misspelt + quoting)
            broken_problems, _ = check_readme.check(
                broken, pathlib.Path("example.cpp"), available, includes
            )

        self.assertEqual(clean_problems, [])
        self.assertEqual(len(broken_problems), 1)
        self.assertIn("libtmuxx.hpp", broken_problems[0])


if __name__ == "__main__":
    unittest.main()
