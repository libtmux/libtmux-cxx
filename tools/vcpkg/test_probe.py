"""Cross-platform regression tests for the vcpkg consumer probe."""

from __future__ import annotations

import contextlib
import io
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

from tools.vcpkg import probe


class ProbeTest(unittest.TestCase):
    """Keep package artifact discovery exact across host platforms."""

    def setUp(self) -> None:
        """Create isolated registry, vcpkg, and consumer fixtures."""
        self.temporary = tempfile.TemporaryDirectory(prefix="vcpkg-probe-test-")
        self.addCleanup(self.temporary.cleanup)
        # Resolved because the probe resolves --keep: Windows hands out an 8.3
        # short path here, and comparing it to the long form fails.
        temporary = pathlib.Path(self.temporary.name).resolve()
        self.root = temporary / "registry"
        self.vcpkg_root = temporary / "vcpkg"
        self.work = temporary / "work"
        (self.root / "examples" / "consume").mkdir(parents=True)
        (self.root / "examples" / "consume" / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.25)\n",
            encoding="utf-8",
        )
        port = self.root / "ports" / "libtmux"
        port.mkdir(parents=True)
        (port / "vcpkg.json").write_text(
            json.dumps(
                {
                    "name": "libtmux",
                    "version-semver": "1.2.3",
                },
            )
            + "\n",
            encoding="utf-8",
        )
        self.vcpkg_root.mkdir()

    def materialize_build(
        self,
        *,
        windows: bool,
        configured: bool,
        triplet: str,
    ) -> tuple[pathlib.Path, pathlib.Path]:
        """Write the artifacts a successful mocked build would produce."""
        build = self.work / "build"
        output = build / probe._BUILD_CONFIGURATION if configured else build
        output.mkdir(parents=True, exist_ok=True)
        executable = output / ("consume.exe" if windows else "consume")
        executable.touch()

        installed = build / "vcpkg_installed" / triplet
        usage = installed / "share" / "libtmux" / "usage"
        usage.parent.mkdir(parents=True)
        usage.write_text(
            "find_package(libtmux COMPONENTS testing)\n"
            "<installed-root>/TRIPLET/tools/libtmux/libtmux-mcp-server "
            "--socket-name NAME\n",
            encoding="utf-8",
        )
        tools = installed / "tools" / "libtmux"
        tools.mkdir(parents=True)
        server = tools / ("libtmux-mcp-server.exe" if windows else "libtmux-mcp-server")
        server.touch()
        return executable, server

    def protocol_response(
        self,
        *,
        windows: bool,
        list_id: int = 2,
    ) -> subprocess.CompletedProcess:
        """Return one complete successful MCP lifecycle exchange."""
        expected = probe._WINDOWS_TOOLS if windows else probe._POSIX_TOOLS
        tools = [
            {
                "name": name,
                "title": name.replace("_", " ").title(),
                "description": f"Describe {name}",
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        key: {
                            "type": probe._TOOL_INPUT_TYPES[name][key],
                            "description": f"Supply {key}",
                        }
                        for key in probe._TOOL_INPUTS[name]
                    },
                    "required": sorted(probe._TOOL_REQUIRED[name]),
                    "additionalProperties": False,
                },
                "outputSchema": {
                    "type": "object",
                    "properties": {
                        key: {"type": probe._TOOL_OUTPUT_TYPES[name][key]}
                        for key in probe._TOOL_OUTPUTS[name]
                    },
                    "required": sorted(probe._TOOL_OUTPUTS[name]),
                    "additionalProperties": False,
                },
                "annotations": {
                    "title": name,
                    "readOnlyHint": name in probe._READ_ONLY_TOOLS,
                    "destructiveHint": name in probe._TERMINAL_TOOLS,
                    "idempotentHint": name in probe._READ_ONLY_TOOLS,
                    "openWorldHint": name in probe._TERMINAL_TOOLS,
                },
            }
            for name in sorted(expected)
        ]
        replies = (
            {
                "jsonrpc": "2.0",
                "id": 1,
                "result": {
                    "protocolVersion": probe._PROTOCOL_VERSION,
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "libtmux-cxx", "version": "1.2.3"},
                    "instructions": "Use the catalog through an explicit route.",
                },
            },
            {"jsonrpc": "2.0", "id": list_id, "result": {"tools": tools}},
        )
        return subprocess.CompletedProcess(
            args=[],
            returncode=0,
            stdout="".join(json.dumps(reply) + "\n" for reply in replies),
            stderr="",
        )

    def legacy_alpha2_response(
        self, *, list_id: int = 2
    ) -> subprocess.CompletedProcess:
        """Return the exact smaller MCP contract shipped by immutable alpha.2."""
        tools = []
        for name in sorted(probe._LEGACY_ALPHA2_TOOLS):
            input_types = probe._LEGACY_ALPHA2_INPUT_TYPES[name]
            tools.append(
                {
                    "name": name,
                    "description": f"Describe legacy {name}",
                    "inputSchema": {
                        "type": "object",
                        "properties": {
                            key: {
                                "type": input_types[key],
                                "description": f"Supply legacy {key}",
                            }
                            for key in input_types
                        },
                        "required": sorted(probe._LEGACY_ALPHA2_REQUIRED[name]),
                        "additionalProperties": False,
                    },
                }
            )
        replies = (
            {
                "jsonrpc": "2.0",
                "id": 1,
                "result": {
                    "protocolVersion": probe._LEGACY_ALPHA2_PROTOCOL_VERSION,
                    "capabilities": {"tools": {}},
                    "serverInfo": {
                        "name": "libtmux",
                        "version": probe._LEGACY_ALPHA2_VERSION,
                    },
                },
            },
            {"jsonrpc": "2.0", "id": list_id, "result": {"tools": tools}},
        )
        return subprocess.CompletedProcess(
            args=[],
            returncode=0,
            stdout="".join(json.dumps(reply) + "\n" for reply in replies),
            stderr="",
        )

    def exercise_run(
        self,
        *,
        windows: bool,
        configured: bool,
        ninja: bool,
        triplet: str,
    ) -> tuple[list[list[str]], pathlib.Path, pathlib.Path, mock.Mock]:
        """Run the probe with a build double that emits platform artifacts."""
        vcpkg = self.vcpkg_root / ("vcpkg.exe" if windows else "vcpkg")
        vcpkg.touch()
        commands: list[list[str]] = []
        artifacts: list[pathlib.Path] = []

        def run_command(
            command: list[str],
            *,
            cwd: pathlib.Path | None = None,
            expected_stdout: str | None = None,
        ) -> int:
            del cwd
            commands.append(command)
            if command[:2] == ["cmake", "--build"]:
                executable, server = self.materialize_build(
                    windows=windows,
                    configured=configured,
                    triplet=triplet,
                )
                artifacts.extend((executable, server))
            if expected_stdout is not None:
                self.assertEqual(expected_stdout, "libtmux 1.2.3 consumed\n")
            return 0

        response = self.protocol_response(windows=windows)
        with (
            mock.patch.object(probe, "_host_is_windows", return_value=windows),
            mock.patch.object(probe, "check_registry", return_value=0),
            mock.patch.object(
                probe,
                "git",
                side_effect=["", "a" * 40, "b" * 40],
            ),
            mock.patch.object(
                probe.shutil,
                "which",
                return_value="/usr/bin/ninja" if ninja else None,
            ),
            mock.patch.object(probe, "_run", side_effect=run_command),
            mock.patch.object(
                probe,
                "_run_protocol",
                return_value=(response, 2, None),
            ) as run_server,
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            status = probe.run(
                self.root,
                self.vcpkg_root,
                triplet=triplet,
                features=["mcp"],
                keep=self.work,
            )

        self.assertEqual(status, 0)
        self.assertEqual(len(artifacts), 2)
        return commands, artifacts[0], artifacts[1], run_server

    def test_linux_single_configuration_commands_and_paths(self) -> None:
        """Use extensionless root outputs from a Ninja Linux build."""
        commands, executable, server, run_server = self.exercise_run(
            windows=False,
            configured=False,
            ninja=True,
            triplet="x64-linux",
        )

        self.assertIn("-DCMAKE_BUILD_TYPE=Release", commands[0])
        generator = commands[0].index("-G")
        self.assertEqual(commands[0][generator : generator + 2], ["-G", "Ninja"])
        self.assertEqual(
            commands[1],
            ["cmake", "--build", str(self.work / "build"), "--config", "Release"],
        )
        self.assertEqual(commands[2], [str(executable)])
        self.assertEqual(executable, self.work / "build" / "consume")
        self.assertEqual(
            run_server.call_args.args[0][:2],
            [str(server), "--socket-name"],
        )
        self.assertRegex(
            run_server.call_args.args[0][2],
            r"^libtmux-vcpkg-probe-[0-9a-f]{16}$",
        )
        self.assertNotIn("notifications/initialized", run_server.call_args.args[1])
        self.assertIn("notifications/initialized", run_server.call_args.args[2])
        self.assertEqual(run_server.call_args.kwargs["version"], "1.2.3")

    def test_windows_multi_configuration_commands_and_paths(self) -> None:
        """Use .exe outputs from the Release directory on native Windows."""
        commands, executable, server, run_server = self.exercise_run(
            windows=True,
            configured=True,
            ninja=True,
            triplet="x64-windows",
        )

        self.assertIn("-DCMAKE_BUILD_TYPE=Release", commands[0])
        self.assertNotIn("-G", commands[0])
        self.assertEqual(
            commands[1],
            ["cmake", "--build", str(self.work / "build"), "--config", "Release"],
        )
        self.assertEqual(commands[2], [str(executable)])
        self.assertEqual(
            executable,
            self.work / "build" / "Release" / "consume.exe",
        )
        self.assertEqual(server.name, "libtmux-mcp-server.exe")
        self.assertEqual(
            run_server.call_args.args[0][:2],
            [str(server), "--socket-name"],
        )
        self.assertRegex(
            run_server.call_args.args[0][2],
            r"^libtmux-vcpkg-probe-[0-9a-f]{16}$",
        )

    def test_executable_selection_rejects_missing_wrong_and_ambiguous(self) -> None:
        """Never guess when a platform artifact is absent or duplicated."""
        directory = self.work / "selection"
        release = directory / "Release"
        release.mkdir(parents=True)

        selected, problem = probe._platform_executable(
            "consumer",
            [directory, release],
            "consume",
            windows=False,
        )
        self.assertIsNone(selected)
        self.assertIn("missing", str(problem))

        (directory / "consume.exe").touch()
        selected, problem = probe._platform_executable(
            "consumer",
            [directory, release],
            "consume",
            windows=False,
        )
        self.assertIsNone(selected)
        self.assertIn("wrong platform suffix", str(problem))

        (directory / "consume").touch()
        selected, problem = probe._platform_executable(
            "consumer",
            [directory, release],
            "consume",
            windows=False,
        )
        self.assertIsNone(selected)
        self.assertIn("ambiguous artifacts", str(problem))

    def test_installed_root_selection_rejects_ambiguous_triplets(self) -> None:
        """Require an explicit triplet when a kept build contains several."""
        build = self.work / "build"
        for triplet in ("arm64-linux", "x64-linux"):
            (build / "vcpkg_installed" / triplet / "share").mkdir(parents=True)

        selected, problem = probe._installed_root(build, None)
        self.assertIsNone(selected)
        self.assertIn("ambiguous target package roots", str(problem))

        selected, problem = probe._installed_root(build, "x64-linux")
        self.assertIsNone(problem)
        self.assertEqual(selected, build / "vcpkg_installed" / "x64-linux")

    def test_installed_tool_rejects_cross_platform_duplicate(self) -> None:
        """Reject a package containing both Windows and POSIX MCP servers."""
        installed = self.work / "installed"
        usage = installed / "share" / "libtmux" / "usage"
        usage.parent.mkdir(parents=True)
        usage.write_text(
            "find_package(libtmux COMPONENTS testing)\n"
            "<installed-root>/TRIPLET/tools/libtmux/libtmux-mcp-server "
            "--socket-name NAME\n"
        )
        tools = installed / "tools" / "libtmux"
        tools.mkdir(parents=True)
        (tools / "libtmux-mcp-server").touch()
        (tools / "libtmux-mcp-server.exe").touch()

        with mock.patch.object(probe, "_run_protocol") as run_server:
            problems = probe._check_installed(
                installed,
                ["mcp"],
                windows=True,
                version="1.2.3",
            )

        self.assertEqual(len(problems), 1)
        self.assertIn("ambiguous artifacts", problems[0])
        run_server.assert_not_called()

    def test_installed_tool_reports_protocol_process_failure(self) -> None:
        """Do not accept stale output from a server that exits unsuccessfully."""
        installed = self.work / "installed-failure"
        usage = installed / "share" / "libtmux" / "usage"
        usage.parent.mkdir(parents=True)
        usage.write_text(
            "find_package(libtmux COMPONENTS testing)\n"
            "<installed-root>/TRIPLET/tools/libtmux/libtmux-mcp-server "
            "--socket-name NAME\n"
        )
        tools = installed / "tools" / "libtmux"
        tools.mkdir(parents=True)
        (tools / "libtmux-mcp-server").touch()
        failed = subprocess.CompletedProcess(
            args=[],
            returncode=7,
            stdout=('{"jsonrpc":"2.0","id":2,"result":{"tools":[{"name":"stale"}]}}\n'),
            stderr="route refused",
        )

        with mock.patch.object(
            probe,
            "_run_protocol",
            return_value=(failed, 2, None),
        ):
            problems = probe._check_installed(
                installed,
                ["mcp"],
                windows=False,
                version="1.2.3",
            )

        self.assertEqual(len(problems), 1)
        self.assertIn("exited 7: route refused", problems[0])

    def test_installed_tool_reports_malformed_protocol_output(self) -> None:
        """Turn invalid server framing into a probe failure, not an exception."""
        installed = self.work / "installed-malformed"
        usage = installed / "share" / "libtmux" / "usage"
        usage.parent.mkdir(parents=True)
        usage.write_text(
            "find_package(libtmux COMPONENTS testing)\n"
            "<installed-root>/TRIPLET/tools/libtmux/libtmux-mcp-server "
            "--socket-name NAME\n"
        )
        tools = installed / "tools" / "libtmux"
        tools.mkdir(parents=True)
        (tools / "libtmux-mcp-server").touch()
        malformed = subprocess.CompletedProcess(
            args=[],
            returncode=0,
            stdout="not-json\n",
            stderr="",
        )

        with mock.patch.object(
            probe,
            "_run_protocol",
            return_value=(malformed, 2, None),
        ):
            problems = probe._check_installed(
                installed,
                ["mcp"],
                windows=False,
                version="1.2.3",
            )

        self.assertEqual(len(problems), 1)
        self.assertIn("malformed JSON reply", problems[0])

    def test_catalog_requires_lifecycle_schema_and_exact_platform_tools(self) -> None:
        """Reject partial lifecycle replies and malformed or stale catalogs."""
        valid = self.protocol_response(windows=False)
        messages = [json.loads(line) for line in valid.stdout.splitlines()]
        cases: list[tuple[str, list[dict[str, object]]]] = [
            ("missing initialize", messages[1:]),
            (
                "initialize error",
                [
                    {"jsonrpc": "2.0", "id": 1, "error": {"code": -1}},
                    messages[1],
                ],
            ),
        ]
        null_name = json.loads(json.dumps(messages))
        null_name[1]["result"]["tools"][0]["name"] = None
        cases.append(("null name", null_name))
        duplicate = json.loads(json.dumps(messages))
        duplicate[1]["result"]["tools"][1]["name"] = duplicate[1]["result"]["tools"][0][
            "name"
        ]
        cases.append(("duplicate", duplicate))
        for label, replies in cases:
            with self.subTest(label=label):
                _, problem = probe._catalog_problem(
                    "".join(json.dumps(reply) + "\n" for reply in replies),
                    windows=False,
                    version="1.2.3",
                    list_id=2,
                )
                self.assertIsNotNone(problem)

        wrong_version = json.loads(json.dumps(messages))
        wrong_version[0]["result"]["serverInfo"]["version"] = "9.9.9"
        _, problem = probe._catalog_problem(
            "".join(json.dumps(reply) + "\n" for reply in wrong_version),
            windows=False,
            version="1.2.3",
            list_id=2,
        )
        self.assertIn("identify", str(problem))

        empty_metadata = json.loads(json.dumps(messages))
        empty_metadata[1]["result"]["tools"][0]["description"] = ""
        _, problem = probe._catalog_problem(
            "".join(json.dumps(reply) + "\n" for reply in empty_metadata),
            windows=False,
            version="1.2.3",
            list_id=2,
        )
        self.assertIn("malformed", str(problem))

        schema_cases = {
            "missing input": ("inputSchema", "properties", "remove"),
            "bad input required": ("inputSchema", "required", [{}]),
            "missing output": ("outputSchema", "properties", "remove"),
            "bad output required": ("outputSchema", "required", [{}]),
            "wrong annotations": ("annotations", "readOnlyHint", False),
        }
        for label, (section, field, value) in schema_cases.items():
            broken = json.loads(json.dumps(messages))
            tool = broken[1]["result"]["tools"][0]
            if value == "remove":
                tool[section][field].pop(next(iter(tool[section][field])))
            else:
                tool[section][field] = value
            with self.subTest(label=label):
                _, problem = probe._catalog_problem(
                    "".join(json.dumps(reply) + "\n" for reply in broken),
                    windows=False,
                    version="1.2.3",
                    list_id=2,
                )
                self.assertIsNotNone(problem)

        _, problem = probe._catalog_problem(
            valid.stdout,
            windows=True,
            version="1.2.3",
            list_id=2,
        )
        self.assertIn("wrong platform", str(problem))

    def test_catalog_accepts_only_the_exact_immutable_alpha2_contract(self) -> None:
        """Keep the published alpha.2 probe strict without inventing new metadata."""
        valid = self.legacy_alpha2_response()
        names, problem = probe._catalog_problem(
            valid.stdout,
            windows=False,
            version=probe._LEGACY_ALPHA2_VERSION,
            list_id=2,
        )
        self.assertIsNone(problem)
        self.assertEqual(set(names), probe._LEGACY_ALPHA2_TOOLS)

        modernized = [json.loads(line) for line in valid.stdout.splitlines()]
        modernized[1]["result"]["tools"][0]["annotations"] = {}
        _, problem = probe._catalog_problem(
            "".join(json.dumps(reply) + "\n" for reply in modernized),
            windows=False,
            version=probe._LEGACY_ALPHA2_VERSION,
            list_id=2,
        )
        self.assertIn("unexpected tool fields", str(problem))

        unexpected_fields = {
            "initialize envelope": (
                (0,),
                "initialize returned unexpected fields",
            ),
            "initialize result": (
                (0, "result"),
                "initialize result returned unexpected fields",
            ),
            "capabilities": (
                (0, "result", "capabilities"),
                "unexpected capabilities",
            ),
            "server info": (
                (0, "result", "serverInfo"),
                "serverInfo returned unexpected fields",
            ),
            "tools envelope": ((1,), "tools/list returned unexpected fields"),
            "tools result": (
                (1, "result"),
                "tools/list result returned unexpected fields",
            ),
            "tool": (
                (1, "result", "tools", 0),
                "unexpected tool fields",
            ),
            "input schema": (
                (1, "result", "tools", 0, "inputSchema"),
                "unexpected input schema fields",
            ),
            "property": (
                (
                    1,
                    "result",
                    "tools",
                    0,
                    "inputSchema",
                    "properties",
                    "target",
                ),
                "malformed properties",
            ),
        }
        for label, (path, expected) in unexpected_fields.items():
            broken = [json.loads(line) for line in valid.stdout.splitlines()]
            node: object = broken
            for component in path:
                if isinstance(component, int):
                    self.assertIsInstance(node, list)
                else:
                    self.assertIsInstance(node, dict)
                node = node[component]
            self.assertIsInstance(node, dict)
            node["surprise"] = True
            with self.subTest(label=label):
                _, problem = probe._catalog_problem(
                    "".join(json.dumps(reply) + "\n" for reply in broken),
                    windows=False,
                    version=probe._LEGACY_ALPHA2_VERSION,
                    list_id=2,
                )
                self.assertIn(expected, str(problem))

        _, problem = probe._catalog_problem(
            valid.stdout,
            windows=True,
            version=probe._LEGACY_ALPHA2_VERSION,
            list_id=2,
        )
        self.assertIn("POSIX-only", str(problem))

    def test_alpha2_probe_uses_one_private_positional_socket_path(self) -> None:
        """Exercise alpha.2 through its immutable positional route contract."""
        installed = self.work / "installed-alpha2"
        usage = installed / "share" / "libtmux" / "usage"
        usage.parent.mkdir(parents=True)
        usage.write_text(
            "find_package(libtmux COMPONENTS testing)\n"
            "<installed-root>/TRIPLET/tools/libtmux/libtmux-mcp-server\n"
            "Later servers use --socket-name NAME\n",
            encoding="utf-8",
        )
        tools = installed / "tools" / "libtmux"
        tools.mkdir(parents=True)
        server = tools / "libtmux-mcp-server"
        server.touch()
        response = self.legacy_alpha2_response()
        route: pathlib.Path | None = None

        def run_protocol(
            command: list[str],
            initialize: str,
            ready: str,
            *,
            version: str,
        ) -> tuple[subprocess.CompletedProcess, int, None]:
            nonlocal route
            self.assertEqual(command[0], str(server))
            self.assertEqual(len(command), 2)
            self.assertNotIn("--socket-name", command)
            route = pathlib.Path(command[1])
            self.assertEqual(route.name, "socket")
            self.assertRegex(route.parent.name, r"^libtmux-vcpkg-mcp-")
            self.assertTrue(route.parent.is_dir())
            self.assertIn(probe._LEGACY_ALPHA2_PROTOCOL_VERSION, initialize)
            self.assertIn("notifications/initialized", ready)
            self.assertEqual(version, probe._LEGACY_ALPHA2_VERSION)
            return response, 2, None

        with (
            mock.patch.object(probe, "_run_protocol", side_effect=run_protocol),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            problems = probe._check_installed(
                installed,
                ["mcp"],
                windows=False,
                version=probe._LEGACY_ALPHA2_VERSION,
            )

        self.assertEqual(problems, [])
        self.assertIsNotNone(route)
        assert route is not None
        self.assertFalse(route.parent.exists())

    def test_protocol_runner_bounds_combined_output(self) -> None:
        """Terminate a server before stdout or stderr can grow without bound."""
        command = [
            sys.executable,
            "-c",
            f"import sys; sys.stdout.write('x' * {probe._PROTOCOL_OUTPUT_LIMIT + 1})",
        ]
        completed, _, problem = probe._run_protocol(
            command,
            "{}\n",
            "",
            version="1.2.3",
            timeout=10,
        )

        self.assertIsNone(completed)
        self.assertIn("output exceeded", str(problem))

    def test_identity_consumer_bounds_output_and_runtime(self) -> None:
        """Do not let a stale consumer hang or fill probe memory."""
        command = [
            sys.executable,
            "-c",
            f"import sys; sys.stdout.write('x' * {probe._PROTOCOL_OUTPUT_LIMIT + 1})",
        ]
        started = time.monotonic()
        with (
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            status = probe._run_expected(
                command,
                cwd=None,
                expected_stdout="",
                timeout=1,
            )

        self.assertEqual(status, 1)
        self.assertLess(time.monotonic() - started, 2.0)

    def test_identity_consumer_accepts_only_windows_crlf_normalization(self) -> None:
        """Accept the native Windows CRT line ending without weakening identity."""
        self.assertTrue(
            probe._identity_stdout_matches("identity\r\n", "identity\n", windows=True)
        )
        self.assertFalse(
            probe._identity_stdout_matches("identity\r\n", "identity\n", windows=False)
        )
        self.assertFalse(
            probe._identity_stdout_matches("other\r\n", "identity\n", windows=True)
        )

    @unittest.skipIf(os.name == "nt", "process-group behavior is POSIX-specific")
    def test_successful_identity_consumer_contains_detached_descendants(self) -> None:
        """Kill same-group descendants even when the identity parent succeeds."""
        self.work.mkdir()
        pid_file = self.work / "identity-child.pid"
        script = (
            "import pathlib, subprocess, sys; "
            "child=subprocess.Popen([sys.executable, '-c', "
            "'import time; time.sleep(30)'], stdin=subprocess.DEVNULL, "
            "stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); "
            f"pathlib.Path({str(pid_file)!r}).write_text(str(child.pid)); "
            "print('identity')"
        )
        with (
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            status = probe._run_expected(
                [sys.executable, "-c", script],
                cwd=None,
                expected_stdout="identity\n",
                timeout=2,
            )

        self.assertEqual(status, 0)
        child = int(pid_file.read_text())
        deadline = time.monotonic() + 1
        while pathlib.Path(f"/proc/{child}").exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertFalse(pathlib.Path(f"/proc/{child}").exists())

    @unittest.skipIf(os.name == "nt", "the fixture uses POSIX select")
    def test_protocol_waits_for_initialize_before_advancing_lifecycle(self) -> None:
        """Prove causal lifecycle and successful process-group containment."""
        self.work.mkdir()
        pid_file = self.work / "protocol-child.pid"
        tools = json.loads(
            self.protocol_response(windows=False).stdout.splitlines()[1]
        )["result"]["tools"]
        script = """
import json
import pathlib
import select
import subprocess
import sys

child = subprocess.Popen(
    [sys.executable, "-c", "import time; time.sleep(30)"],
    stdin=subprocess.DEVNULL,
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
)
pathlib.Path(PID_FILE).write_text(str(child.pid))

first = sys.stdin.readline()
if select.select([sys.stdin], [], [], 0)[0]:
    sys.stderr.write("PREINITIALIZE_BYTES\\n")
reply = {
    "jsonrpc": "2.0",
    "id": 1,
    "result": {
        "protocolVersion": "2024-11-05",
        "capabilities": {"tools": {}},
        "serverInfo": {"name": "libtmux-cxx", "version": "1.2.3"},
        "instructions": "Use an explicit route.",
    },
}
print(json.dumps(reply), flush=True)
sys.stdin.readline()
request = json.loads(sys.stdin.readline())
listed = {"jsonrpc": "2.0", "id": request["id"], "result": {"tools": TOOLS}}
print(json.dumps(listed), flush=True)
""".replace("TOOLS", repr(tools)).replace("PID_FILE", repr(str(pid_file)))
        initialize = '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}\n'
        ready = '{"jsonrpc":"2.0","method":"notifications/initialized"}\n'

        completed, list_id, problem = probe._run_protocol(
            [sys.executable, "-c", script],
            initialize,
            ready,
            version="1.2.3",
            timeout=2,
        )

        self.assertIsNone(problem)
        self.assertIsNotNone(completed)
        assert completed is not None
        self.assertNotIn("PREINITIALIZE_BYTES", completed.stderr)
        _, catalog_problem = probe._catalog_problem(
            completed.stdout,
            windows=False,
            version="1.2.3",
            list_id=int(list_id),
        )
        self.assertIsNone(catalog_problem)
        child = int(pid_file.read_text())
        deadline = time.monotonic() + 1
        while pathlib.Path(f"/proc/{child}").exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertFalse(pathlib.Path(f"/proc/{child}").exists())

    def test_protocol_rejects_tools_reply_emitted_before_request(self) -> None:
        """An unpredictable request id binds tools/list to the sent request."""
        response = self.protocol_response(windows=False).stdout.splitlines()
        script = """
import sys

sys.stdin.readline()
print(INITIALIZED, flush=True)
print(PREMATURE, flush=True)
sys.stdin.readline()
sys.stdin.readline()
""".replace("INITIALIZED", repr(response[0])).replace(
            "PREMATURE",
            repr(response[1]),
        )

        with mock.patch.object(probe.secrets, "randbelow", return_value=41):
            completed, _, problem = probe._run_protocol(
                [sys.executable, "-c", script],
                "{}\n",
                "{}\n",
                version="1.2.3",
                timeout=1,
            )

        self.assertIsNone(completed)
        self.assertIn("tools/list", str(problem))

    @unittest.skipIf(os.name == "nt", "process-group behavior is POSIX-specific")
    def test_protocol_timeout_contains_descendants_holding_output_pipes(self) -> None:
        """Return promptly when an exited server leaves inherited pipes open."""
        script = (
            "import subprocess, sys; "
            "subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)']); "
            "sys.exit(0)"
        )
        started = time.monotonic()

        completed, _, problem = probe._run_protocol(
            [sys.executable, "-c", script],
            "{}\n",
            "",
            version="1.2.3",
            timeout=0.1,
        )

        self.assertLess(time.monotonic() - started, 2.0)
        self.assertIsNone(completed)
        self.assertIsNotNone(problem)

    def test_protocol_requires_tools_reply_before_stdin_eof(self) -> None:
        """Reject a server that cannot answer a persistent stdio client."""
        response = self.protocol_response(windows=False).stdout.splitlines()
        script = """
import sys

sys.stdin.readline()
print(INITIALIZED, flush=True)
sys.stdin.read()
print(LISTED, flush=True)
""".replace("INITIALIZED", repr(response[0])).replace(
            "LISTED",
            repr(response[1]),
        )
        started = time.monotonic()

        completed, _, problem = probe._run_protocol(
            [sys.executable, "-c", script],
            "{}\n",
            "{}\n{}\n",
            version="1.2.3",
            timeout=0.2,
        )

        self.assertLess(time.monotonic() - started, 2.0)
        self.assertIsNone(completed)
        self.assertIn("waiting for tools/list", str(problem))

    def test_keep_must_be_fresh_and_outside_both_repositories(self) -> None:
        """Never overwrite files in either source tree or an existing path."""
        (self.vcpkg_root / "vcpkg").touch()
        candidates = (
            self.root,
            self.root / "nested",
            self.root.parent,
            self.vcpkg_root,
            self.vcpkg_root / "nested",
            self.work,
        )
        self.work.mkdir()

        for candidate in candidates:
            with (
                self.subTest(candidate=candidate),
                mock.patch.object(probe, "git") as git,
                contextlib.redirect_stdout(io.StringIO()),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                self.assertEqual(
                    probe.run(
                        self.root,
                        self.vcpkg_root,
                        keep=candidate,
                    ),
                    2,
                )
                git.assert_not_called()

        self.assertFalse((self.root / "vcpkg.json").exists())
        self.assertFalse((self.vcpkg_root / "vcpkg.json").exists())

    def test_inconsistent_registry_is_rejected_before_any_build(self) -> None:
        """Never claim to test dirty port bytes through a committed baseline."""
        (self.vcpkg_root / "vcpkg").touch()
        with (
            mock.patch.object(probe, "check_registry", return_value=1),
            mock.patch.object(probe, "git") as git,
            mock.patch.object(probe, "_run") as run_command,
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            self.assertEqual(
                probe.run(
                    self.root,
                    self.vcpkg_root,
                    keep=self.work,
                ),
                2,
            )

        git.assert_not_called()
        run_command.assert_not_called()
        self.assertFalse(self.work.exists())

    def test_dirty_versions_are_rejected_before_any_build(self) -> None:
        """Resolve only bytes represented by the committed registry baseline."""
        (self.vcpkg_root / "vcpkg").touch()
        with (
            mock.patch.object(probe, "check_registry", return_value=0),
            mock.patch.object(probe, "git", return_value=" M versions/baseline.json"),
            mock.patch.object(probe, "_run") as run_command,
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            self.assertEqual(
                probe.run(
                    self.root,
                    self.vcpkg_root,
                    keep=self.work,
                ),
                2,
            )

        run_command.assert_not_called()
        self.assertFalse(self.work.exists())


if __name__ == "__main__":
    unittest.main()
