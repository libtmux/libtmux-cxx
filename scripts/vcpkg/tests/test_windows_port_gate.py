"""The Windows release gate fails on each way the port can misreport support."""

from __future__ import annotations

import os
import pathlib

import pytest
import windows_port_gate as gate


class Recorder:
    """A command runner that answers from a script and keeps what it was asked."""

    def __init__(self, root: pathlib.Path, *, admit: str = "", fail: str = "") -> None:
        """Answer as a correct port does, except for the named triplet or step."""
        self.root = root
        self.admit = admit
        self.fail = fail
        self.commands: list[list[str]] = []
        self.environments: list[dict[str, str] | None] = []

    def __call__(
        self, command: list[str], environment: dict[str, str] | None
    ) -> gate.Result:
        """Record ``command`` and answer it."""
        self.commands.append(command)
        self.environments.append(environment)
        joined = " ".join(command)
        if "--dry-run" in command:
            if self.admit and self.admit in joined:
                return gate.Result(0, "would install")
            return gate.Result(1, "error: libtmux is only supported on windows")
        if "install" in command and any(
            a.startswith("--x-install-root=") for a in command
        ):
            root = next(a for a in command if a.startswith("--x-install-root="))
            triplet = pathlib.Path(root.split("=", 1)[1]).name.removeprefix("vcpkg-")
            if self.fail != "no-server":
                server = (
                    self.root / "build" / f"vcpkg-{triplet}" / triplet / "tools/libtmux"
                )
                server.mkdir(parents=True, exist_ok=True)
                (server / "libtmux-mcp-server.exe").write_text("")
        if self.fail and self.fail in joined and self.fail != "no-server":
            return gate.Result(2, "boom")
        return gate.Result(0, "ok")


def test_a_correct_port_passes_every_triplet(tmp_path: pathlib.Path) -> None:
    """Each unsupported triplet is refused and each supported one is consumed."""
    recorder = Recorder(tmp_path)
    passed = gate.gate(tmp_path / "vcpkg", tmp_path, recorder)
    assert passed[: len(gate.UNSUPPORTED)] == [f"refuses {t}" for t in gate.UNSUPPORTED]
    assert passed[len(gate.UNSUPPORTED) :] == [
        f"installs and consumes {t}" for t in gate.SUPPORTED
    ]


def test_the_static_triplet_alone_uses_the_static_runtime(
    tmp_path: pathlib.Path,
) -> None:
    """Consumers of the others link the runtime as a DLL."""
    recorder = Recorder(tmp_path)
    gate.gate(tmp_path / "vcpkg", tmp_path, recorder)
    runtimes = {
        a.split("=", 1)[1]
        for command in recorder.commands
        for a in command
        if a.startswith("-DCMAKE_MSVC_RUNTIME_LIBRARY=")
    }
    assert runtimes == {"MultiThreaded", "MultiThreadedDLL"}
    assert gate.runtime_library("x64-windows-static") == "MultiThreaded"
    assert gate.runtime_library("x64-windows-static-md") == "MultiThreadedDLL"


def test_the_consumer_runs_with_the_installed_bin_first_on_path(
    tmp_path: pathlib.Path,
) -> None:
    """The DLL triplets need the installed ``bin`` to find the library."""
    recorder = Recorder(tmp_path)
    gate.gate(tmp_path / "vcpkg", tmp_path, recorder)
    paths = [e["PATH"] for e in recorder.environments if e is not None]
    expected = [
        str(tmp_path / "build" / f"vcpkg-{triplet}" / triplet / "bin")
        for triplet in gate.SUPPORTED
    ]
    assert [path.split(os.pathsep)[0] for path in paths] == expected


@pytest.mark.parametrize(
    ("admit", "fail", "message"),
    [
        ("x64-uwp", "", "admitted x64-uwp"),
        ("", "no-server", "installed no MCP server"),
        ("", "testing-component", "testing-component rejection"),
        ("", "--config", "building the"),
        (
            "",
            "libtmux[mcp]:x64-windows-static",
            "the transitioned port for x64-windows-static",
        ),
    ],
)
def test_each_way_the_port_can_misreport_support_is_named(
    tmp_path: pathlib.Path, admit: str, fail: str, message: str
) -> None:
    """A triplet it admits, a missing server or a failing step stops the gate."""
    recorder = Recorder(tmp_path, admit=admit, fail=fail)
    with pytest.raises(gate.GateError, match=message):
        gate.gate(tmp_path / "vcpkg", tmp_path, recorder)
