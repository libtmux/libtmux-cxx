"""Regression tests for the README region-isolation checker."""

from __future__ import annotations

import unittest

from tools.docs import check_example_isolation as isolation


class DeclarationsTest(unittest.TestCase):
    """`declarations()` reads a region's own assumptions, not its compiler."""

    def test_splits_multiple_bindings_on_semicolons(self) -> None:
        """More than one Given binding becomes more than one parameter."""
        body = (
            "// Given: const libtmux::Pane& pane; const libtmux::Server& server\n"
            "act(pane, server);\n"
        )

        given, without = isolation.declarations(body)

        self.assertEqual(
            given, ["const libtmux::Pane& pane", "const libtmux::Server& server"]
        )
        self.assertEqual(without.strip(), "act(pane, server);")

    def test_a_body_with_no_given_line_declares_nothing(self) -> None:
        """A self-contained region is unaffected by this convention."""
        body = "libtmux::Chain chain;\n"

        given, without = isolation.declarations(body)

        self.assertEqual(given, [])
        self.assertEqual(without, body)

    def test_an_auto_typed_given_is_refused(self) -> None:
        """`auto` would make the check pass without checking anything.

        `program()` places each Given declaration as a function parameter;
        an `auto` one makes that function an unconstrained template, which
        `-fsyntax-only` accepts without ever instantiating (and so without
        ever checking) its body.
        """
        body = "// Given: auto panes\nact(panes);\n"

        with self.assertRaises(SystemExit):
            isolation.declarations(body)


class ProgramTest(unittest.TestCase):
    """The synthesized translation unit places Given bindings as parameters."""

    def test_given_bindings_become_the_region_functions_parameters(self) -> None:
        """A signature a reader never sees still gates what the region uses."""
        source = isolation.program(
            includes=["<libtmux/libtmux.hpp>"],
            given=["const libtmux::Server& server"],
            body="server.sessions();",
        )

        self.assertIn("static int region(const libtmux::Server& server) {", source)
        self.assertIn("server.sessions();", source)


if __name__ == "__main__":
    unittest.main()
