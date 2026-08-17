"""Install the port the way a consumer does, from the registry, and use it.

The check beside this one proves the versions database and the ports agree.
It cannot prove a consumer can resolve them: that needs a real vcpkg, a real
manifest naming this repository as a git registry, and a real build against
whatever comes back.

So this writes the two files a consumer writes, points them at this checkout,
and builds `examples/consume` -- the smallest complete consumer, which needs no
tmux server -- through the vcpkg toolchain. What it exercises that nothing else
does is resolution: the baseline commit, the git-tree behind it, and the
package config arriving somewhere `find_package` looks.
"""

from __future__ import annotations

import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import typing as t

from tools.vcpkg.check import port_version
from tools.vcpkg.git import git


def _run(command: list[str], *, cwd: pathlib.Path | None = None) -> int:
    """Run a command, streaming its output, and return its status."""
    printable = " ".join(command)
    print(f"$ {printable}", flush=True)
    return subprocess.run(command, cwd=cwd, check=False).returncode


def _manifest(port: str, version: str, features: list[str], builtin: str) -> str:
    """Render the vcpkg.json a consumer writes."""
    dependency: dict[str, t.Any] | str = port
    if features:
        dependency = {"name": port, "features": features}
    return json.dumps(
        {
            "name": "libtmux-registry-probe",
            "version": version,
            "builtin-baseline": builtin,
            "dependencies": [dependency],
        },
        indent=2,
    )


def _configuration(port: str, repository: str, baseline: str) -> str:
    """Render the configuration that names this repository as a registry."""
    return json.dumps(
        {
            "registries": [
                {
                    "kind": "git",
                    "repository": repository,
                    "baseline": baseline,
                    "packages": [port],
                },
            ],
        },
        indent=2,
    )


def _check_installed(
    installed: pathlib.Path,
    port: str,
    features: list[str],
) -> list[str]:
    """Report what the resolved package failed to deliver."""
    problems: list[str] = []

    usage = installed / "share" / port / "usage"
    if not usage.is_file():
        problems.append(f"{usage}: the port installs no usage text")
    elif "COMPONENTS testing" not in usage.read_text():
        problems.append(
            f"{usage}: does not show the testing component, so it is the "
            f"heuristic text vcpkg generates rather than the port's own",
        )

    if "mcp" in features:
        tool = installed / "tools" / port / "libtmux-mcp-server"
        if not tool.is_file():
            problems.append(f"{tool}: the mcp feature installed no server")
        else:
            hello = (
                '{"jsonrpc":"2.0","id":1,"method":"initialize","params":'
                '{"protocolVersion":"2024-11-05","capabilities":{},'
                '"clientInfo":{"name":"probe","version":"1"}}}\n'
                '{"jsonrpc":"2.0","id":2,"method":"tools/list"}\n'
            )
            done = subprocess.run(
                [str(tool)],
                input=hello,
                capture_output=True,
                text=True,
                check=False,
                timeout=60,
            )
            names = [
                tool_entry.get("name")
                for line in done.stdout.splitlines()
                if line.strip()
                for tool_entry in json.loads(line).get("result", {}).get("tools", [])
            ]
            if not names:
                problems.append(f"{tool}: answered no tools over stdio")
            else:
                print(f"the installed server advertises: {', '.join(names)}")

    return problems


def run(
    root: pathlib.Path,
    vcpkg_root: pathlib.Path,
    *,
    port: str = "libtmux",
    triplet: str | None = None,
    features: list[str] | None = None,
    keep: pathlib.Path | None = None,
) -> int:
    """Resolve the port from this repository as a registry, and build on it."""
    features = features or []
    vcpkg = vcpkg_root / "vcpkg"
    if not vcpkg.is_file():
        print(f"{vcpkg}: no vcpkg binary; bootstrap it first", file=sys.stderr)
        return 2

    consumer = root / "examples" / "consume"
    if not (consumer / "CMakeLists.txt").is_file():
        print(f"{consumer}: missing, and it is the fixture", file=sys.stderr)
        return 2

    declared = port_version(root / "ports" / port)
    if declared is None:
        print(f"ports/{port}: declares no version", file=sys.stderr)
        return 2
    _, version, _ = declared

    baseline = git("rev-parse", "HEAD", repo=root)
    builtin = git("rev-parse", "HEAD", repo=vcpkg_root)
    if not baseline or not builtin:
        print("both repositories must have a HEAD commit", file=sys.stderr)
        return 2

    context = (
        tempfile.TemporaryDirectory(prefix="libtmux-vcpkg-probe-")
        if keep is None
        else None
    )
    work = keep if keep is not None else pathlib.Path(t.cast("t.Any", context).name)
    work.mkdir(parents=True, exist_ok=True)

    try:
        (work / "vcpkg.json").write_text(
            _manifest(port, version, features, builtin) + "\n",
        )
        (work / "vcpkg-configuration.json").write_text(
            _configuration(port, str(root), baseline) + "\n",
        )
        print(f"registry {root} @ {baseline[:12]} -> {port} {version}")

        build = work / "build"
        configure = [
            "cmake",
            "-S",
            str(consumer),
            "-B",
            str(build),
            f"-DCMAKE_TOOLCHAIN_FILE={vcpkg_root / 'scripts/buildsystems/vcpkg.cmake'}",
            f"-DVCPKG_MANIFEST_DIR={work}",
        ]
        if shutil.which("ninja"):
            configure += ["-G", "Ninja"]
        if triplet:
            configure.append(f"-DVCPKG_TARGET_TRIPLET={triplet}")
        if _run(configure) != 0:
            print("error: the consumer did not configure", file=sys.stderr)
            return 1

        if _run(["cmake", "--build", str(build)]) != 0:
            print("error: the consumer did not build", file=sys.stderr)
            return 1

        if _run([str(build / "consume")]) != 0:
            print("error: the consumer built but did not run", file=sys.stderr)
            return 1

        installed = next((build / "vcpkg_installed").glob("*/share"), None)
        if installed is None:
            print("error: nothing was installed", file=sys.stderr)
            return 1

        problems = _check_installed(installed.parent, port, features)
        if problems:
            for problem in problems:
                print(f"error: {problem}", file=sys.stderr)
            return 1
    finally:
        if context is not None:
            context.cleanup()

    asked = f"{port}[{','.join(features)}]" if features else port
    print(f"resolved and consumed {asked} {version} from the registry")
    return 0
