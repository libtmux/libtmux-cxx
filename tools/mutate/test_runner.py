"""Tests for mutation-runner evidence selection."""

from __future__ import annotations

import hashlib
import json
import pathlib
import subprocess
import tempfile
import unittest

from tools.mutate.runner import Mutation, _fingerprint, run


class MutationRunnerTest(unittest.TestCase):
    """Require the runner to prove both its binary and test selection."""

    def test_fingerprint_finds_a_windows_executable(self) -> None:
        """Recognize the executable suffix produced by native Windows builds."""
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            executable = (
                root
                / "build"
                / "windows-psmux"
                / "tests"
                / "windows"
                / "Debug"
                / "libtmux_windows_psmux_smoke.exe"
            )
            executable.parent.mkdir(parents=True)
            executable.write_bytes(b"windows executable")

            self.assertEqual(
                _fingerprint(root, "windows-psmux", "libtmux_windows_psmux_smoke"),
                hashlib.sha256(b"windows executable").hexdigest(),
            )

    def test_run_can_fingerprint_an_output_named_differently_from_its_target(
        self,
    ) -> None:
        """Track an executable whose OUTPUT_NAME differs from its CMake target."""
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "guard.cpp"
            source.write_text("guard = true;\n", encoding="utf-8")
            executable = root / "build" / "cxx-dev" / "libtmux-mcp-server"
            executable.parent.mkdir(parents=True)
            executable.write_bytes(b"before")
            builds = 0
            tests = 0

            def execute(argv: list[str]) -> subprocess.CompletedProcess[bytes]:
                nonlocal builds, tests
                if argv[0] == "cmake":
                    builds += 1
                    if builds == 2:
                        executable.write_bytes(b"after")
                    elif builds == 3:
                        executable.write_bytes(b"restored")
                    return subprocess.CompletedProcess(argv, 0, b"", b"")
                if "--show-only=json-v1" in argv:
                    listing = json.dumps({"tests": [{"name": "protocol"}]}).encode()
                    return subprocess.CompletedProcess(argv, 0, listing, b"")
                tests += 1
                return subprocess.CompletedProcess(
                    argv, 1 if tests == 2 else 0, b"", b""
                )

            outcome = run(
                Mutation(
                    mutation_id="renamed-output",
                    path="guard.cpp",
                    find="true",
                    replace="false",
                    target="libtmux_mcp_server",
                    executable="libtmux-mcp-server",
                    guards="the renamed executable",
                    test_regex=r"^protocol$",
                ),
                root,
                "cxx-dev",
                runner=execute,
            )

            self.assertEqual(outcome.verdict, "killed")

    def test_run_uses_the_explicit_test_selector(self) -> None:
        """Run a target whose CTest name cannot be inferred from its target."""
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "guard.cpp"
            source.write_text("guard = true;\n", encoding="utf-8")
            executable = (
                root
                / "build"
                / "windows-psmux"
                / "Debug"
                / "libtmux_windows_psmux_smoke.exe"
            )
            executable.parent.mkdir(parents=True)
            executable.write_bytes(b"before")
            commands: list[list[str]] = []
            builds = 0
            test_runs = 0

            def execute(argv: list[str]) -> subprocess.CompletedProcess[bytes]:
                nonlocal builds, test_runs
                commands.append(argv)
                if argv[0] == "cmake":
                    builds += 1
                    if builds == 2:
                        executable.write_bytes(b"after")
                    elif builds == 3:
                        executable.write_bytes(b"restored")
                    return subprocess.CompletedProcess(argv, 0, b"", b"")
                if "--show-only=json-v1" in argv:
                    listing = json.dumps(
                        {"tests": [{"name": "windows.psmux-smoke"}]}
                    ).encode()
                    return subprocess.CompletedProcess(argv, 0, listing, b"")
                test_runs += 1
                status = 1 if test_runs == 2 else 0
                return subprocess.CompletedProcess(argv, status, b"", b"")

            selector = r"^windows[.]psmux-smoke$"
            outcome = run(
                Mutation(
                    mutation_id="windows-guard",
                    path="guard.cpp",
                    find="true",
                    replace="false",
                    target="libtmux_windows_psmux_smoke",
                    guards="the Windows guard",
                    test_regex=selector,
                ),
                root,
                "windows-psmux",
                runner=execute,
            )

            self.assertEqual(outcome.verdict, "killed")
            self.assertEqual(source.read_text(encoding="utf-8"), "guard = true;\n")
            selected = [command for command in commands if command[0] == "ctest"]
            self.assertEqual(len(selected), 4)
            for command in selected:
                self.assertEqual(command[command.index("--tests-regex") + 1], selector)

    def test_empty_test_selection_is_not_a_kill(self) -> None:
        """Do not mistake CTest's no-tests failure for a killed mutation."""
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "guard.cpp"
            source.write_text("guard = true;\n", encoding="utf-8")
            executable = root / "build" / "cxx-dev" / "guard_test"
            executable.parent.mkdir(parents=True)
            executable.write_bytes(b"before")

            def execute(argv: list[str]) -> subprocess.CompletedProcess[bytes]:
                if argv[0] == "cmake":
                    return subprocess.CompletedProcess(argv, 0, b"", b"")
                listing = json.dumps({"tests": []}).encode()
                return subprocess.CompletedProcess(argv, 0, listing, b"")

            outcome = run(
                Mutation(
                    mutation_id="unselected-guard",
                    path="guard.cpp",
                    find="true",
                    replace="false",
                    target="guard_test",
                    guards="the guard",
                    test_regex=r"^missing$",
                ),
                root,
                "cxx-dev",
                runner=execute,
            )

            self.assertEqual(outcome.verdict, "not a result")
            self.assertEqual(outcome.detail, "the test selector matched no tests")
            self.assertEqual(source.read_text(encoding="utf-8"), "guard = true;\n")

    def test_red_baseline_is_not_a_kill(self) -> None:
        """Refuse a verdict when the selected test was already failing."""
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "guard.cpp"
            source.write_text("guard = true;\n", encoding="utf-8")
            executable = root / "build" / "cxx-dev" / "guard_test"
            executable.parent.mkdir(parents=True)
            executable.write_bytes(b"before")

            def execute(argv: list[str]) -> subprocess.CompletedProcess[bytes]:
                if argv[0] == "cmake":
                    return subprocess.CompletedProcess(argv, 0, b"", b"")
                if "--show-only=json-v1" in argv:
                    listing = json.dumps({"tests": [{"name": "guard"}]}).encode()
                    return subprocess.CompletedProcess(argv, 0, listing, b"")
                return subprocess.CompletedProcess(argv, 1, b"failed", b"")

            outcome = run(
                Mutation(
                    mutation_id="red-baseline",
                    path="guard.cpp",
                    find="true",
                    replace="false",
                    target="guard_test",
                    guards="the guard",
                    test_regex=r"^guard$",
                ),
                root,
                "cxx-dev",
                runner=execute,
            )

            self.assertEqual(outcome.verdict, "not a result")
            self.assertEqual(outcome.detail, "the selected tests already fail")
            self.assertEqual(source.read_text(encoding="utf-8"), "guard = true;\n")


if __name__ == "__main__":
    unittest.main()
