#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
r"""Hold the release's vcpkg port to what Windows claims, on the Windows runner.

After ``vcpkg_registry.py release`` enables Windows, the transitioned port must
refuse every triplet it does not support, and for each triplet it does support
install, serve the MCP server, refuse the POSIX-only ``testing`` component, and
build and run an installed consumer. ``release.yml`` runs this once, in the
Windows release job.

    uv run scripts/vcpkg/windows_port_gate.py --vcpkg C:\vcpkg
"""

from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import sys
import typing as t

UNSUPPORTED = (
    "arm64-android-release",
    "arm64-ios",
    "x86-windows",
    "arm64-windows",
    "x64-freebsd",
    "x64-uwp",
    "x64-mingw-dynamic",
    "x64-xbox-scarlett",
)
SUPPORTED = (
    "x64-windows",
    "x64-windows-release",
    "x64-windows-static",
    "x64-windows-static-md",
)
STATIC_RUNTIME = "x64-windows-static"
REFUSAL = "is only supported on"
GENERATOR = ("-G", "Visual Studio 17 2022", "-A", "x64")


class GateError(Exception):
    """The transitioned port did not behave as Windows support claims."""


class Result(t.NamedTuple):
    """What a finished command printed, stdout and stderr together."""

    status: int
    output: str


Runner = t.Callable[[list[str], dict[str, str] | None], Result]


def runtime_library(triplet: str) -> str:
    """Return the MSVC runtime a consumer of ``triplet`` must use."""
    return "MultiThreaded" if triplet == STATIC_RUNTIME else "MultiThreadedDLL"


def real_runner(command: list[str], environment: dict[str, str] | None) -> Result:
    """Run ``command``, echoing its output as it ends, and return it."""
    done = subprocess.run(
        command,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    sys.stdout.write(done.stdout)
    return Result(done.returncode, done.stdout)


def must_succeed(run: Runner, command: list[str], what: str) -> None:
    """Run ``command`` and raise unless it exits 0."""
    result = run(command, None)
    if result.status != 0:
        message = f"{what} failed ({result.status})"
        raise GateError(message)


def gate(
    vcpkg: pathlib.Path,
    root: pathlib.Path,
    run: Runner = real_runner,
    *,
    probe: pathlib.Path | None = None,
) -> list[str]:
    """Check every unsupported and supported triplet and return what passed."""
    executable = str(vcpkg / "vcpkg.exe")
    overlay = f"--overlay-ports={root / 'ports'}"
    probe = probe or root / "scripts/installed_probe/installed_probe.py"
    passed = []
    for triplet in UNSUPPORTED:
        result = run(
            [
                executable,
                "install",
                "--classic",
                "--dry-run",
                f"libtmux:{triplet}",
                overlay,
            ],
            None,
        )
        if result.status == 0 or REFUSAL not in result.output:
            message = f"the transitioned port admitted {triplet}\n{result.output}"
            raise GateError(message)
        passed.append(f"refuses {triplet}")
    for triplet in SUPPORTED:
        installed = root / "build" / f"vcpkg-{triplet}"
        prefix = installed / triplet
        must_succeed(
            run,
            [
                executable,
                "install",
                "--classic",
                f"libtmux[mcp]:{triplet}",
                overlay,
                f"--x-install-root={installed}",
            ],
            f"the transitioned port for {triplet}",
        )
        server = prefix / "tools/libtmux/libtmux-mcp-server.exe"
        if not server.is_file():
            message = f"the {triplet} port installed no MCP server: {server}"
            raise GateError(message)
        options = [
            *GENERATOR,
            f"-DCMAKE_TOOLCHAIN_FILE={vcpkg / 'scripts/buildsystems/vcpkg.cmake'}",
            f"-DVCPKG_TARGET_TRIPLET={triplet}",
            f"-DVCPKG_INSTALLED_DIR={installed}",
            "-DVCPKG_MANIFEST_MODE=OFF",
            f"-DCMAKE_MSVC_RUNTIME_LIBRARY={runtime_library(triplet)}",
        ]
        must_succeed(
            run,
            [sys.executable, str(probe), "testing-component", "--", *options],
            f"the {triplet} testing-component rejection",
        )
        consumer = root / "build" / f"consume-{triplet}"
        must_succeed(
            run,
            [
                "cmake",
                "-S",
                str(root / "examples/consume"),
                "-B",
                str(consumer),
                *options,
            ],
            f"configuring the {triplet} package consumer",
        )
        must_succeed(
            run,
            ["cmake", "--build", str(consumer), "--config", "Release"],
            f"building the {triplet} package consumer",
        )
        environment = {
            **os.environ,
            "PATH": os.pathsep.join([str(prefix / "bin"), os.environ.get("PATH", "")]),
        }
        ran = run([str(consumer / "Release/consume.exe")], environment)
        if ran.status != 0:
            message = f"the {triplet} package consumer failed ({ran.status})"
            raise GateError(message)
        passed.append(f"installs and consumes {triplet}")
    return passed


def main(argv: t.Sequence[str] | None = None) -> int:
    """Run the gate and return a process status."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vcpkg", type=pathlib.Path, required=True)
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path())
    arguments = parser.parse_args(argv)
    try:
        passed = gate(arguments.vcpkg.resolve(), arguments.root.resolve())
    except GateError as error:
        print(f"windows_port_gate: {error}", file=sys.stderr)
        return 1
    print("\n".join(passed))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
