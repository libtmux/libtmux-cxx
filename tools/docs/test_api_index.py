"""Regression tests for the public API-reference parser."""

from __future__ import annotations

import pathlib
import unittest

from tools.docs import api_index

FIXTURE_DIR = pathlib.Path(__file__).with_name("fixtures")
FIXTURE = FIXTURE_DIR / "api_index.hpp"
EXPECTED = FIXTURE_DIR / "api_index.expected.md"


class ApiIndexTest(unittest.TestCase):
    """Keep scope handling and navigation stable on focused declarations."""

    def test_fixture_matches_golden_reference(self) -> None:
        """Render all supported declaration shapes deterministically."""
        rendered = (
            "\n".join(
                api_index._render_header(FIXTURE, "fixture/api_index.hpp")
            ).rstrip()
            + "\n"
        )
        self.assertEqual(EXPECTED.read_text(encoding="utf-8"), rendered)

    def test_bodies_and_private_members_are_not_public_symbols(self) -> None:
        """Never mistake implementation scope for a declaration scope."""
        _, sections = api_index.read_header(FIXTURE)
        rendered = repr(sections)
        self.assertNotIn("body_only", rendered)
        self.assertNotIn("constructor_body_only", rendered)
        self.assertNotIn("hidden_free_function", rendered)
        self.assertNotIn("void hidden", rendered)
        self.assertNotIn("T value_", rendered)

    def test_constructor_stops_before_initializer_list(self) -> None:
        """Document a constructor signature without its definition body."""
        _, sections = api_index.read_header(FIXTURE)
        box = next(section for section in sections if section.name == "Box")
        constructor = next(entry for entry in box.entries if entry.symbol == "Box")
        self.assertEqual("explicit Box(T value)", constructor.signature)

    def test_enum_members_are_anchored_entries_with_comments(self) -> None:
        """Expose compact and documented enumerators as caller-facing symbols."""
        _, sections = api_index.read_header(FIXTURE)
        mode = next(section for section in sections if section.name == "Mode")
        documented = next(
            section for section in sections if section.name == "DocumentedMode"
        )
        self.assertEqual("enum class DocumentedMode : unsigned", documented.declaration)
        self.assertEqual(["direct", "queued"], [entry.symbol for entry in mode.entries])
        self.assertEqual(
            ["direct = 1", "queued = 2"],
            [entry.signature for entry in mode.entries],
        )
        self.assertEqual(
            [["Execute immediately."], ["Wait until work is available."]],
            [entry.prose for entry in documented.entries],
        )

    def test_lambda_initializer_body_is_elided(self) -> None:
        """Keep a constant's initializer shape without executable statements."""
        rendered = "\n".join(api_index._render_header(FIXTURE, "fixture/api_index.hpp"))
        self.assertIn("[](int value) { /* implementation omitted */ }", rendered)
        self.assertNotIn("return value + 1", rendered)

    def test_statement_macro_body_is_elided(self) -> None:
        """Keep a statement macro's parameters without its implementation."""
        rendered = "\n".join(api_index._render_header(FIXTURE, "fixture/api_index.hpp"))
        self.assertIn(
            "#define FIXTURE_CHECK(value) /* implementation omitted */", rendered
        )
        self.assertNotIn("if (!(value))", rendered)


if __name__ == "__main__":
    unittest.main()
