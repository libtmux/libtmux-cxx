"""Regression tests for the changelog check.

Each rule is shown failing on a copy of the real files, one break at a time,
beside the unbroken copy, so a check that stopped looking would fail here
instead of passing every changelog quietly.
"""

from __future__ import annotations

import pathlib
import shutil
import tempfile
import unittest

from tools.docs import check_changelogs as changelogs

ROOT = changelogs.ROOT
MCP = "apps/mcp/CHANGELOG.md"
WORKSPACE = "apps/workspace/CHANGELOG.md"
MCP_RULES = "apps/mcp/CMakeLists.txt"


class Copy(unittest.TestCase):
    """A throwaway tree holding what the check reads."""

    def setUp(self) -> None:
        """Copy each changelog and each program's CMake file into a fresh tree."""
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = pathlib.Path(directory.name)
        for path, cmake in changelogs.PACKAGES.values():
            for name in (path, cmake):
                if name is not None:
                    (self.root / name).parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy(ROOT / name, self.root / name)

    def edit(self, name: str, old: str, new: str) -> None:
        """Replace the first `old` in the copy of `name`, which has to hold it."""
        file = self.root / name
        text = file.read_text(encoding="utf-8")
        self.assertIn(old, text)
        file.write_text(text.replace(old, new, 1), encoding="utf-8")

    def reported(self) -> list[str]:
        """Return what the check says about the copy."""
        return changelogs.failures(self.root)


class TheRepository(unittest.TestCase):
    """The tree itself, which every break below is measured against."""

    def test_passes(self) -> None:
        """The repository keeps and installs every changelog."""
        self.assertEqual(changelogs.failures(ROOT), [])


class Policy(Copy):
    """Each rule, broken one way at a time."""

    def test_a_missing_changelog_is_reported_for_every_package(self) -> None:
        """Deleting any one changelog is named."""
        for name, (path, _) in changelogs.PACKAGES.items():
            with self.subTest(package=name):
                (self.root / path).unlink()
                self.assertIn(f"{path}: {name} has no changelog", self.reported())
                shutil.copy(ROOT / path, self.root / path)

    def test_an_unreleased_heading_is_required_outside_a_fence(self) -> None:
        """A heading inside a fence does not count, whichever way the fence is made."""
        for fence, inner in (
            ("```", "~~~"),
            ("~~~", ""),
            ("````", "```"),
            ("```", "```js"),
        ):
            with self.subTest(fence=fence, inner=inner):
                shutil.copy(ROOT / MCP, self.root / MCP)
                self.edit(
                    MCP,
                    "## Unreleased\n",
                    f"{fence}md\n{inner}\n## Unreleased\n{fence}\n",
                )
                self.assertIn(f'{MCP}: has no "## Unreleased" section', self.reported())

    def test_a_changelog_without_an_unreleased_heading_is_reported(self) -> None:
        """A file with no Unreleased section is named."""
        self.edit(WORKSPACE, "## Unreleased\n", "")
        self.assertIn(f'{WORKSPACE}: has no "## Unreleased" section', self.reported())

    def test_another_packages_changelog_pasted_over_one_is_reported(self) -> None:
        """The library's file pasted over a program's is named as the wrong package."""
        shutil.copy(self.root / "CHANGELOG.md", self.root / WORKSPACE)
        self.assertIn(
            f"{WORKSPACE}: is the changelog of libtmux, not tmux-workspace",
            self.reported(),
        )

    def test_the_opening_sentence_must_come_first(self) -> None:
        """The sentence that names the package is the first thing after the title."""
        self.edit(MCP, "# Changelog\n\n", "# Changelog\n\nSomething else first.\n\n")
        self.assertIn(
            f'{MCP}: does not open with "Notable changes to `libtmux-mcp-server`."',
            self.reported(),
        )

    def test_a_program_must_install_its_own_changelog_under_share_doc(self) -> None:
        """A rule that names another file, another package's, or elsewhere fails."""
        source = '"${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md"'
        for path, cmake, name in (
            (MCP, MCP_RULES, "libtmux-mcp-server"),
            (WORKSPACE, "apps/workspace/CMakeLists.txt", "tmux-workspace"),
        ):
            for label, old, new in (
                ("another file", source, '"${CMAKE_CURRENT_SOURCE_DIR}/README.md"'),
                (
                    "another package's file",
                    source,
                    '"${PROJECT_SOURCE_DIR}/CHANGELOG.md"',
                ),
                ("another directory", f"doc/{name}", "doc/other"),
                ("no rule at all", "install(FILES", "message(STATUS"),
            ):
                with self.subTest(cmake=cmake, break_=label):
                    shutil.copy(ROOT / cmake, self.root / cmake)
                    self.edit(cmake, old, new)
                    self.assertTrue(
                        any(
                            f"{cmake}: does not install {path} under share/doc/{name}"
                            in line
                            for line in self.reported()
                        )
                    )

    def test_a_commented_out_install_rule_installs_nothing(self) -> None:
        """A rule inside a CMake comment does not count, nor does a trailing one."""
        for old, new in (
            ("install(FILES ", "# install(FILES "),
            (
                'install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md"',
                'install(FILES README.md # "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md"',
            ),
        ):
            with self.subTest(new=new):
                shutil.copy(ROOT / MCP_RULES, self.root / MCP_RULES)
                self.edit(MCP_RULES, old, new)
                self.assertTrue(
                    any("does not install" in line for line in self.reported())
                )

    def test_a_rule_after_a_trailing_comment_is_not_a_rule(self) -> None:
        """Text after a `#` on a line of other CMake is a comment, whatever it says."""
        rule = (
            'install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md"\n'
            "            DESTINATION "
            '"${CMAKE_INSTALL_DATAROOTDIR}/doc/libtmux-mcp-server")'
        )
        text = (self.root / MCP_RULES).read_text(encoding="utf-8")
        self.assertIn(rule, text)
        (self.root / MCP_RULES).write_text(
            text.replace(rule, "message(STATUS x) # " + rule.replace("\n", " ")),
            encoding="utf-8",
        )
        self.assertTrue(any("does not install" in line for line in self.reported()))

    def test_the_destination_must_be_under_the_data_root(self) -> None:
        """A changelog installed under `bin` is not beside the documentation."""
        self.edit(
            MCP_RULES,
            "${CMAKE_INSTALL_DATAROOTDIR}/doc/libtmux-mcp-server",
            "${CMAKE_INSTALL_BINDIR}/doc/libtmux-mcp-server",
        )
        self.assertTrue(any("does not install" in line for line in self.reported()))

    def test_text_before_the_title_is_not_an_opening(self) -> None:
        """The policy sentence only counts when the file starts with the title."""
        self.edit(MCP, "# Changelog\n", "Preamble first.\n\n# Changelog\n")
        self.assertTrue(any("does not open with" in line for line in self.reported()))

    def test_a_release_heading_inside_a_fence_is_not_a_release(self) -> None:
        """A heading-shaped line in a code block does not add a release."""
        self.edit(MCP, "## Unreleased\n", "```md\n## 9.9.9\n```\n\n## Unreleased\n")
        self.assertEqual(self.reported(), [])

    def test_a_package_that_lists_other_releases_is_reported(self) -> None:
        """The packages share one version, so every file lists the same releases."""
        self.edit(MCP, "## 0.1.0 (", "## 0.1.0-draft (")
        self.assertTrue(
            any(
                "lists different releases than CHANGELOG.md" in line
                for line in self.reported()
            )
        )


if __name__ == "__main__":
    unittest.main()
