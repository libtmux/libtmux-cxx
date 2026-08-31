"""Tests for mutation-runner restoration guarantees."""

from __future__ import annotations

import pathlib
import subprocess
import unittest
from unittest import mock

from tools.mutate import __main__ as mutation_main
from tools.mutate.runner import Mutation, Outcome


class MutationMainTest(unittest.TestCase):
    """Keep a green mutation report from hiding a broken restored tree."""

    def operation_mutation(self, mutation_id: str) -> Mutation:
        """Return one named operation-state mutation."""
        return next(
            mutation
            for mutation in mutation_main.CATALOGUE
            if mutation.mutation_id == mutation_id
        )

    def mutation_source(self, mutation_id: str) -> tuple[Mutation, str]:
        """Return one mutation and the source text it must match."""
        mutation = self.operation_mutation(mutation_id)
        repository = pathlib.Path(__file__).parents[2]
        source = (repository / mutation.path).read_text(encoding="utf-8")
        return mutation, source

    def test_operation_mutations_cover_the_foundation_guards(self) -> None:
        """Keep the four operation-state race ratchets selectable."""
        operation_ids = {
            mutation.mutation_id
            for mutation in mutation_main.CATALOGUE
            if mutation.mutation_id.startswith("operation-")
        }

        self.assertTrue(
            {
                "operation-single-publication",
                "operation-cancellation-source-owned",
                "operation-registration-recheck",
                "operation-source-retention",
            }.issubset(operation_ids)
        )

    def test_single_publication_mutation_matches_the_current_guard(self) -> None:
        """Keep the publication ratchet bound to one current source guard."""
        mutation, source = self.mutation_source("operation-single-publication")

        self.assertEqual(source.count(mutation.find), 1)

    def test_cancellation_mutation_targets_the_templated_state(self) -> None:
        """Keep outcome publication out of the non-template relay mutation."""
        mutation, source = self.mutation_source("operation-cancellation-source-owned")

        occurrence = source.find(mutation.find)
        operation_state = source.index(
            "template <typename T> class OperationState final"
        )
        self.assertEqual(source.count(mutation.find), 1)
        self.assertGreater(occurrence, operation_state)

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

    def test_mcp_mutations_build_the_protocol_test(self) -> None:
        """Build the CTest executable as well as its separately named server."""
        mutations = {
            mutation.mutation_id: mutation
            for mutation in mutation_main.CATALOGUE
            if mutation.mutation_id
            in {"mcp-batch-duplicate-preflight", "mcp-id-held-through-write"}
        }

        self.assertEqual(len(mutations), 2)
        for mutation in mutations.values():
            self.assertEqual(mutation.target, "mcp_protocol_test")
            self.assertEqual(mutation.executable, "libtmux-mcp-server")
            self.assertEqual(mutation.test_regex, r"^consumer[.]mcp[.]protocol$")


if __name__ == "__main__":
    unittest.main()
