"""Tests for mutation-runner restoration guarantees."""

from __future__ import annotations

import subprocess
import unittest
from unittest import mock

from tools.mutate import __main__ as mutation_main
from tools.mutate.runner import Outcome


class MutationMainTest(unittest.TestCase):
    """Keep a green mutation report from hiding a broken restored tree."""

    def run_main_with_restore_status(self, status: int) -> int:
        """Run one mocked mutation and return the command status."""
        mutation = mutation_main.CATALOGUE[0]
        completed = subprocess.CompletedProcess(
            args=["cmake"],
            returncode=status,
            stdout="",
            stderr="synthetic rebuild failure" if status else "",
        )
        with (
            mock.patch.object(
                mutation_main,
                "run",
                return_value=Outcome(mutation, "killed"),
            ),
            mock.patch.object(mutation_main.subprocess, "run", return_value=completed),
        ):
            return mutation_main.main(["--id", mutation.mutation_id])

    def test_success_requires_the_restored_tree_to_build(self) -> None:
        """Report success only after the final unmutated rebuild passes."""
        self.assertEqual(self.run_main_with_restore_status(0), 0)

    def test_failed_restored_build_fails_the_run(self) -> None:
        """Reject an otherwise killed catalogue when restoration is red."""
        self.assertEqual(self.run_main_with_restore_status(1), 2)

    def test_run_all_selects_only_mutations_for_the_preset(self) -> None:
        """Keep Windows-only targets out of the ordinary POSIX catalogue run."""
        general = mutation_main.CATALOGUE[0]
        windows = next(
            mutation
            for mutation in mutation_main.CATALOGUE
            if mutation.presets == ("windows-psmux",)
        )
        completed = subprocess.CompletedProcess(
            args=["cmake"], returncode=0, stdout="", stderr=""
        )
        with (
            mock.patch.object(mutation_main, "CATALOGUE", (general, windows)),
            mock.patch.object(
                mutation_main,
                "run",
                side_effect=lambda mutation, *_args, **_kwargs: Outcome(
                    mutation, "killed"
                ),
            ) as run_mutation,
            mock.patch.object(mutation_main.subprocess, "run", return_value=completed),
        ):
            status = mutation_main.main(["--preset", "cxx-dev"])

        self.assertEqual(status, 0)
        run_mutation.assert_called_once()
        self.assertEqual(run_mutation.call_args.args[0], general)

    def test_explicit_incompatible_mutation_fails_closed(self) -> None:
        """Do not silently omit a named mutation that cannot run in this build."""
        windows = next(
            mutation
            for mutation in mutation_main.CATALOGUE
            if mutation.presets == ("windows-psmux",)
        )
        with (
            mock.patch.object(mutation_main, "CATALOGUE", (windows,)),
            mock.patch.object(mutation_main, "run") as run_mutation,
            mock.patch.object(mutation_main.subprocess, "run") as rebuild,
        ):
            status = mutation_main.main(
                ["--preset", "cxx-dev", "--id", windows.mutation_id]
            )

        self.assertEqual(status, 2)
        run_mutation.assert_not_called()
        rebuild.assert_not_called()

    def test_unknown_mutation_id_fails_closed(self) -> None:
        """Reject misspelt selections instead of running an accidental subset."""
        with (
            mock.patch.object(mutation_main, "run") as run_mutation,
            mock.patch.object(mutation_main.subprocess, "run") as rebuild,
        ):
            status = mutation_main.main(["--id", "does-not-exist"])

        self.assertEqual(status, 2)
        run_mutation.assert_not_called()
        rebuild.assert_not_called()


if __name__ == "__main__":
    unittest.main()
