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


if __name__ == "__main__":
    unittest.main()
