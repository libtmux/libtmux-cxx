"""Regression tests for the README link checker.

Each rule is shown failing beside a clean control, so a checker that stopped
looking would fail here instead of passing every README quietly.
"""

from __future__ import annotations

import pathlib
import tempfile
import textwrap
import unittest

from tools.docs import check_readme_links as links

REFERENCE = links.REFERENCE
KNOWN = {"Server": "libtmux-server", "Pane": "libtmux-pane"}


class Fixture:
    """A throwaway checkout holding the files a README links to."""

    def __init__(self, directory: str) -> None:
        self.root = pathlib.Path(directory)
        (self.root / "docs").mkdir()
        (self.root / "docs" / "guide.md").write_text(
            "# Guide\n\n## A section\n", encoding="utf-8"
        )
        (self.root / "include").mkdir()

    def readme(self, text: str) -> pathlib.Path:
        """Write a README into the checkout and return its path."""
        path = self.root / "README.md"
        path.write_text(textwrap.dedent(text), encoding="utf-8")
        return path

    def check(self, text: str, *, introductions: bool = False) -> list[str]:
        """Every problem the checker reports for a README with this text."""
        path = self.readme(text)
        return [
            problem.message
            for problem in links.check_readme(
                path, self.root, KNOWN, introductions=introductions
            )
        ]


class ResolutionTest(unittest.TestCase):
    """A link must name something that is in the checkout."""

    def check(self, text: str) -> list[str]:
        """Check a README in a fresh checkout."""
        with tempfile.TemporaryDirectory() as directory:
            return Fixture(directory).check(text)

    def test_accepts_links_that_resolve(self) -> None:
        """A file, a directory, a heading and an absolute repository path pass."""
        found = self.check(
            """\
            # Title

            See [guide](docs/guide.md#a-section), [headers](include/), and
            [here](#title). The tree is at
            [github](https://github.com/libtmux/libtmux-cxx/tree/master/include).
            """
        )
        self.assertEqual(found, [])

    def test_refuses_a_missing_relative_file(self) -> None:
        """A relative link to a file that is not there is named."""
        found = self.check("[gone](docs/missing.md)\n")
        self.assertEqual(len(found), 1)
        self.assertIn("docs/missing.md", found[0])

    def test_refuses_a_missing_heading(self) -> None:
        """A fragment that no heading produces is named."""
        found = self.check("[x](docs/guide.md#no-such-section)\n")
        self.assertEqual(len(found), 1)
        self.assertIn("no-such-section", found[0])

    def test_refuses_a_missing_heading_in_the_same_file(self) -> None:
        """A `#fragment` link is held to the README's own headings."""
        found = self.check("# Title\n\n[x](#elsewhere)\n")
        self.assertEqual(len(found), 1)
        self.assertIn("elsewhere", found[0])

    def test_refuses_a_repository_url_for_a_path_that_is_not_there(self) -> None:
        """The absolute tree link a package heading uses must name a directory."""
        found = self.check(
            "[x](https://github.com/libtmux/libtmux-cxx/tree/master/apps/nope)\n"
        )
        self.assertEqual(len(found), 1)
        self.assertIn("apps/nope", found[0])

    def test_follows_a_reference_style_link(self) -> None:
        """A link written as `[text][label]` is held to its definition."""
        found = self.check("[x][gone]\n\n[gone]: docs/missing.md\n")
        self.assertEqual(len(found), 1)

    def test_ignores_links_inside_code_fences(self) -> None:
        """A path shown in a code block is an example, not a link."""
        found = self.check("```md\n[x](docs/missing.md)\n```\n")
        self.assertEqual(found, [])

    def test_slug_matches_the_anchors_github_makes(self) -> None:
        """Punctuation drops, spaces become hyphens, `&` leaves two."""
        self.assertEqual(links.slug("Requirements & support"), "requirements--support")
        self.assertEqual(links.slug("`libtmux::testing`"), "libtmuxtesting")
        self.assertEqual(links.slug("[`tmux-workspace`](u)"), "tmux-workspace")


class IntroductionTest(unittest.TestCase):
    """An API is linked where it is introduced, and its page linked once."""

    def check(self, text: str) -> list[str]:
        """Check a README for first-mention links."""
        with tempfile.TemporaryDirectory() as directory:
            return Fixture(directory).check(text, introductions=True)

    server = f"[`Server`][server]\n\n[server]: {REFERENCE}libtmux-server/\n"

    def test_accepts_a_linked_first_mention(self) -> None:
        """A type linked to its page the first time prose names it is clean."""
        self.assertEqual(self.check("Open a " + self.server), [])

    def test_refuses_an_unlinked_first_mention(self) -> None:
        """The first prose mention of a public type must be a link."""
        found = self.check("Open a `Server` here, then " + self.server)
        self.assertEqual(len(found), 1)
        self.assertIn("first prose mention of `Server`", found[0])
        self.assertIn(f"{REFERENCE}libtmux-server/", found[0])

    def test_refuses_a_type_whose_page_is_never_linked(self) -> None:
        """Naming a type in prose without ever linking its page is reported."""
        found = self.check("Open a `Server` here.\n")
        self.assertTrue(any("never linked" in message for message in found))

    def test_accepts_a_member_page_as_the_type_page(self) -> None:
        """`Server::from_env` links to `libtmux-server-from_env/`."""
        found = self.check(
            "Use [`Server::from_env()`][env].\n\n"
            f"[env]: {REFERENCE}libtmux-server-from_env/\n"
        )
        self.assertEqual(found, [])

    def test_a_link_elsewhere_satisfies_the_first_mention_not_the_page(self) -> None:
        """An anchor at the first mention still needs the page linked somewhere."""
        found = self.check("# T\n\nSee [`Server`](#t).\n")
        self.assertEqual(len(found), 1)
        self.assertIn("never linked", found[0])

    def test_ignores_names_in_code_fences_tables_and_headings(self) -> None:
        """Only prose introduces an API; indexes and examples do not."""
        found = self.check(
            """\
            ### The `Server` heading

            | Type | Role |
            |---|---|
            | `Server` | entry point |

            ```cpp
            // `Server` in a comment
            ```
            """
        )
        self.assertEqual(found, [])

    def test_ignores_names_that_are_not_public_symbols(self) -> None:
        """A backticked word that is no header declares is not an API mention."""
        self.assertEqual(self.check("Run `cmake` and read `Notes`.\n"), [])

    def test_symbols_come_from_the_generated_api_pages(self) -> None:
        """A top-level entry names a symbol; a member entry does not."""
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "docs").mkdir()
            (root / "docs" / "api.md").write_text(
                "- [`Server`](#a)\n  - [`Server::run`](#b)\n", encoding="utf-8"
            )
            (root / "docs" / "api-testing.md").write_text(
                "- [`ScopedTmuxServer`](#c)\n", encoding="utf-8"
            )

            found = links.symbols(root)

        self.assertEqual(
            found,
            {
                "Server": "libtmux-server",
                "ScopedTmuxServer": "libtmux-test-scopedtmuxserver",
            },
        )


class ToolLinkTest(unittest.TestCase):
    """A tool name in a table row or list item links to its page."""

    page = "https://libtmux.org/en/cxx/latest/mcp/tools/list_panes/"
    definition = f"[tool-list_panes]: {page}\n"

    def check(self, text: str) -> list[str]:
        """Check a README that declares one tool."""
        with tempfile.TemporaryDirectory() as directory:
            return Fixture(directory).check(text)

    def test_accepts_linked_tool_names(self) -> None:
        """A name linked through its definition is clean in a row and a bullet."""
        found = self.check(
            "| [`list_panes`][tool-list_panes] | all |\n\n"
            "- [`list_panes`][tool-list_panes]\n\n" + self.definition
        )
        self.assertEqual(found, [])

    def test_refuses_an_unlinked_name_in_a_table_row(self) -> None:
        """A bare tool name in a table is named with the line it is on."""
        found = self.check("| `list_panes` | all |\n\n" + self.definition)
        self.assertEqual(len(found), 1)
        self.assertIn("line 1: tool `list_panes`", found[0])

    def test_refuses_an_unlinked_name_in_a_list_item(self) -> None:
        """The same name in a bullet is held to the same rule."""
        found = self.check("- `list_panes` lists panes\n\n" + self.definition)
        self.assertEqual(len(found), 1)

    def test_leaves_prose_and_other_code_alone(self) -> None:
        """Only tables and lists are indexes; a sentence may name a tool bare."""
        found = self.check(
            "Call `list_panes` first.\n\n- `--socket-name NAME`\n\n" + self.definition
        )
        self.assertEqual(found, [])

    def test_refuses_a_definition_that_points_elsewhere(self) -> None:
        """A `[tool-NAME]` label must name that tool's page."""
        found = self.check(
            "[tool-list_panes]: https://libtmux.org/en/cxx/latest/mcp/tools/other/\n"
        )
        self.assertEqual(len(found), 1)
        self.assertIn("not " + self.page, found[0])


class OnlineTest(unittest.TestCase):
    """`--online` reports any external link that does not answer 200."""

    def test_reports_only_the_links_that_fail(self) -> None:
        """A 404 is named with its status; a 200 is not mentioned."""
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "README.md"
            path.write_text(
                "[ok](https://example.org/ok)\n[gone](https://example.org/gone)\n",
                encoding="utf-8",
            )

            found = links.check_online(
                [path], fetch=lambda url: 200 if url.endswith("/ok") else 404
            )

        self.assertEqual(len(found), 1)
        self.assertIn("https://example.org/gone answered 404", found[0].message)


if __name__ == "__main__":
    unittest.main()
