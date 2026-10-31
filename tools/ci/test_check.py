"""Tests for the CI drift guards.

The first test runs the guards on this repository. Every other test breaks one
thing in a copy of it and expects the guard for that thing to say so, because
a guard that cannot fail is not guarding anything.
"""

from __future__ import annotations

import json
import pathlib
import shutil
import tempfile
import unittest

from tools.ci import check

ROOT = pathlib.Path(__file__).resolve().parents[2]
COPIED = (
    "README.md",
    "docs/compatibility.md",
    ".github/CONTRIBUTING.md",
    "tools/ci/tmux_matrix.json",
)
SHA = "35b54afac29c97fb54faba5b513f8fbd1882f113"


class CheckTest(unittest.TestCase):
    """Break a copy of the repository and watch the guard notice."""

    def setUp(self) -> None:
        """Copy the files the guards read."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        for name in COPIED:
            self.copy(name)
        (self.root / ".github/workflows").mkdir(parents=True, exist_ok=True)
        for source in (ROOT / ".github/workflows").glob("*.yml"):
            self.copy(f".github/workflows/{source.name}")

    def copy(self, name: str) -> None:
        """Copy one repository file into the scratch tree."""
        target = self.root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(ROOT / name, target)

    def edit(self, name: str, old: str, new: str, count: int = 1) -> None:
        """Replace ``old`` with ``new`` in a copied file, which must contain it."""
        path = self.root / name
        text = path.read_text()
        self.assertEqual(text.count(old), count, f"{old!r} in {name}")
        path.write_text(text.replace(old, new))

    def problems(self) -> list[str]:
        """Return what the guards say about the scratch tree."""
        return check.check(self.root)

    def assertFlags(self, fragment: str) -> None:
        """Assert some problem mentions ``fragment``."""
        problems = self.problems()
        self.assertTrue(
            any(fragment in problem for problem in problems),
            f"no problem mentions {fragment!r}: {problems}",
        )

    def test_the_repository_agrees_with_its_manifest(self) -> None:
        """The pages and workflows in this checkout pass every guard."""
        self.assertEqual(check.check(ROOT), [])
        self.assertEqual(self.problems(), [])

    def test_a_new_release_in_the_manifest_reaches_every_page(self) -> None:
        """Adding 3.9 without editing the docs names each page."""
        path = self.root / "tools/ci/tmux_matrix.json"
        manifest = json.loads(path.read_text())
        manifest["released"].append("3.9")
        path.write_text(json.dumps(manifest))
        problems = self.problems()
        for page in check.PAGES:
            self.assertTrue(any(page in p and "3.9" in p for p in problems), page)

    def test_a_page_with_another_range_is_flagged(self) -> None:
        """A README that still says 3.7 is the newest is caught."""
        self.edit(
            "README.md",
            "from 3.3a\n  through 3.8, plus `master`",
            "from 3.3a\n  through 3.7c, plus `master`",
        )
        self.assertFlags("README.md says CI covers 3.3a through 3.7c")

    def test_a_page_without_the_range_is_flagged(self) -> None:
        """A page must state the range at all."""
        self.edit("docs/compatibility.md", "through 3.8, plus", "through 3.8 and")
        self.assertFlags("docs/compatibility.md does not say CI covers")

    def test_a_page_without_the_edges_is_flagged(self) -> None:
        """The page must say what a push runs."""
        self.edit("docs/compatibility.md", "3.3a\nand 3.8", "3.3a\nand 3.7c")
        self.assertFlags("docs/compatibility.md does not say a push runs the edges")

    def test_a_page_with_other_refusal_cells_is_flagged(self) -> None:
        """The refusal list is read from the manifest too."""
        self.edit(
            ".github/CONTRIBUTING.md", "3.0a, 3.1c, 3.2a and 3.3", "3.1c, 3.2a and 3.3"
        )
        self.assertFlags("does not list the refusal cells")

    def test_contributing_must_show_the_dispatch_command(self) -> None:
        """The section needs the exact command."""
        self.edit(
            ".github/CONTRIBUTING.md",
            "gh workflow run ci.yml",
            "gh workflow run x.yml",
            3,
        )
        self.assertFlags("gh workflow run ci.yml")

    def test_a_version_list_in_the_workflow_is_flagged(self) -> None:
        """A hard-coded second list would drift from the manifest."""
        self.edit(
            ".github/workflows/ci.yml",
            "  windows:\n",
            "  windows:\n    env:\n      OLD: '3.4'\n",
        )
        self.assertFlags("ci.yml names tmux 3.4")

    def test_an_input_spliced_into_a_script_is_flagged(self) -> None:
        """Inputs reach scripts through env only."""
        self.edit(
            ".github/workflows/ci.yml",
            "          tmux -V\n          read -r",
            "          echo ${{ inputs.tmux-ref }}\n          read -r",
        )
        self.assertFlags("splices an input into a script")

    def test_a_matrix_value_spliced_into_a_script_is_flagged(self) -> None:
        """A tmux ref from the matrix is also user input."""
        self.edit(
            ".github/workflows/ci.yml",
            "          tmux -V\n          read -r",
            "          echo ${{ matrix.tmux }}\n          read -r",
        )
        self.assertFlags("splices an input into a script")

    def test_a_bare_off_in_the_tmate_input_is_flagged(self) -> None:
        """A bare off is false to GitHub, which breaks every default dispatch."""
        self.edit(".github/workflows/ci.yml", "options: ['off',", "options: [off,")
        self.assertFlags("must quote 'off'")

    def test_a_bare_off_default_is_flagged(self) -> None:
        """The default has to be the same string as the option it names."""
        self.edit(".github/workflows/ci.yml", "default: 'off'", "default: off")
        self.assertFlags("must quote 'off'")

    def test_an_unpinned_tmate_is_flagged(self) -> None:
        """The action is pinned by commit."""
        self.edit(
            ".github/workflows/ci.yml",
            f"action-tmate@{SHA} # v3.24",
            "action-tmate@v3 # v3",
            2,
        )
        self.assertFlags("not a full commit")

    def test_a_tmate_open_to_everyone_is_flagged(self) -> None:
        """Access is limited to the dispatching account."""
        self.edit(
            ".github/workflows/ci.yml",
            "limit-access-to-actor: true",
            "limit-access-to-actor: false",
            2,
        )
        self.assertFlags("does not limit access")

    def test_a_tmate_without_a_timeout_is_flagged(self) -> None:
        """A session cannot hold a runner forever."""
        self.edit(
            ".github/workflows/ci.yml",
            "        timeout-minutes: 30\n        with:\n          limit",
            "        with:\n          limit",
            2,
        )
        self.assertFlags("no timeout-minutes")

    def test_a_tmate_outside_a_dispatch_is_flagged(self) -> None:
        """A push can never open a session."""
        self.edit(
            ".github/workflows/ci.yml",
            "if: github.event_name == 'workflow_dispatch' && matrix.tmate == 'before",
            "if: matrix.tmate == 'before",
        )
        self.assertFlags("outside a workflow_dispatch")

    def test_a_third_tmate_step_is_flagged(self) -> None:
        """A session outside the compatibility job is not allowed."""
        path = self.root / ".github/workflows/ci.yml"
        text = path.read_text()
        step = (
            f"      - uses: mxschmitt/action-tmate@{SHA}\n"
            "        with:\n          limit-access-to-actor: true\n"
        )
        path.write_text(
            text.replace("  toolchains:\n", "  toolchains:\n    steps_marker: x\n", 1)
            + step
        )
        self.assertFlags("outside the compatibility job")

    def test_tmate_in_another_workflow_is_flagged(self) -> None:
        """Release, docs and publish jobs never open a session."""
        path = self.root / ".github/workflows/release.yml"
        path.write_text(
            path.read_text() + f"      - uses: mxschmitt/action-tmate@{SHA}\n"
        )
        self.assertFlags("release.yml runs action-tmate")

    def test_a_job_missing_from_the_aggregate_is_flagged(self) -> None:
        """A new job that nobody gated would pass unnoticed."""
        self.edit(
            ".github/workflows/ci.yml",
            "      - fuzz\n      - locale\n",
            "      - locale\n",
        )
        self.assertFlags("all-checks-passed does not need fuzz")

    def test_a_stale_name_in_the_aggregate_is_flagged(self) -> None:
        """A renamed job left in `needs` would fail every run."""
        self.edit(
            ".github/workflows/ci.yml",
            "      - fuzz\n      - locale\n",
            "      - fuzz\n      - fuzzing\n      - locale\n",
        )
        self.assertFlags("needs fuzzing, which is not a job")

    def test_a_shared_concurrency_group_is_flagged(self) -> None:
        """Dispatch runs must not cancel one another."""
        self.edit(
            ".github/workflows/ci.yml",
            "${{ github.event_name == 'workflow_dispatch' && format('-{0}', "
            "github.run_id) || '' }}",
            "",
        )
        self.assertFlags("concurrency group")

    def test_a_matrix_that_ignores_the_plan_is_flagged(self) -> None:
        """The compatibility matrix is the plan's output."""
        self.edit(
            ".github/workflows/ci.yml",
            "matrix: ${{ fromJSON(needs.plan.outputs.compat) }}",
            "matrix:\n        tmux: [x]",
        )
        self.assertFlags("compatibility job does not take its matrix from the plan")

    def test_a_release_that_runs_the_edges_is_flagged(self) -> None:
        """A release must prove the whole table."""
        self.edit(".github/workflows/release.yml", "scope: full", "scope: edges")
        self.assertFlags("does not run ci.yml at scope full")

    def test_a_release_that_does_not_wait_for_ci_is_flagged(self) -> None:
        """Publishing waits for the full-scope run."""
        self.edit(".github/workflows/release.yml", "needs: [ci]", "needs: []")
        self.assertFlags("does not wait for ci")

    def test_a_release_trusting_an_old_run_is_flagged(self) -> None:
        """The run-history check proves only what that run covered."""
        path = self.root / ".github/workflows/release.yml"
        path.write_text(path.read_text() + "# gh api actions/workflows/ci.yml/runs\n")
        self.assertFlags("trusts an earlier ci run")


if __name__ == "__main__":
    unittest.main()
