"""Check that the versions database still describes the ports beside it.

This repository is a vcpkg git registry: ``ports/`` holds the ports and
``versions/`` records, per version, the git-tree each one was published from.
A consumer names a commit of this repository as their baseline, and vcpkg
resolves the port from the tree that commit's ``versions/`` points at -- not
from ``ports/`` directly. Edit a portfile without re-running ``x-add-version``
and the registry serves the old tree while the repository shows the new one.

``vcpkg x-add-version`` reports that disagreement, but it exits 0 while doing
so and writes nothing, so neither a status check nor a diff of ``versions/``
notices. This asserts the invariant those two miss:

    the git-tree recorded for a port's declared version
    == the git-tree of ``ports/<name>`` at HEAD

Run from the repository root::

    python3 -m tools.vcpkg
"""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
import typing as t

# vcpkg accepts one of these per manifest, and treats the choice as part of the
# version's identity: a port declaring `version-semver` is not interchangeable
# with the same string under `version`.
VERSION_FIELDS: t.Final[tuple[str, ...]] = (
    "version",
    "version-semver",
    "version-string",
    "version-date",
)


def git(*args: str, repo: pathlib.Path) -> str:
    """Run a git command in ``repo`` and return its stripped stdout."""
    done = subprocess.run(
        ["git", *args],
        cwd=repo,
        capture_output=True,
        text=True,
        check=False,
    )
    if done.returncode != 0:
        return ""
    return done.stdout.strip()


def declared_version(manifest: dict[str, t.Any]) -> tuple[str, str, int] | None:
    """Return ``(field, version, port_version)`` a manifest declares, if any."""
    for field in VERSION_FIELDS:
        if field in manifest:
            return field, str(manifest[field]), int(manifest.get("port-version", 0))
    return None


def check_port(
    root: pathlib.Path,
    port: pathlib.Path,
) -> list[str]:
    """Report every way ``port`` and its versions file disagree."""
    problems: list[str] = []
    name = port.name

    manifest_path = port / "vcpkg.json"
    if not manifest_path.is_file():
        return [f"ports/{name}/: no vcpkg.json, so it is not a port"]

    manifest = json.loads(manifest_path.read_text())
    if manifest.get("name") != name:
        problems.append(
            f"ports/{name}/vcpkg.json: declares the name "
            f"{manifest.get('name')!r}, but vcpkg resolves a port by its "
            f"directory name",
        )

    declared = declared_version(manifest)
    if declared is None:
        fields = ", ".join(VERSION_FIELDS)
        return [f"ports/{name}/vcpkg.json: declares none of {fields}"]
    field, version, port_version = declared

    versions_path = root / "versions" / f"{name[0]}-" / f"{name}.json"
    if not versions_path.is_file():
        return [
            (
                f"{versions_path.relative_to(root)}: missing. Run "
                f"`python3 -m tools.vcpkg --how` for the command that writes it."
            ),
        ]

    entries = json.loads(versions_path.read_text()).get("versions", [])
    matching = [
        entry
        for entry in entries
        if entry.get(field) == version and entry.get("port-version", 0) == port_version
    ]
    if not matching:
        published = (
            ", ".join(
                f"{entry.get(field, '?')}#{entry.get('port-version', 0)}"
                for entry in entries
            )
            or "nothing"
        )
        problems.append(
            f"{versions_path.relative_to(root)}: has no entry for "
            f"{version}#{port_version}, which ports/{name}/vcpkg.json declares. "
            f"It publishes {published}.",
        )
    elif len(matching) > 1:
        problems.append(
            f"{versions_path.relative_to(root)}: {len(matching)} entries for "
            f"{version}#{port_version}; a version is published once.",
        )
    else:
        recorded = matching[0].get("git-tree")
        actual = git("rev-parse", f"HEAD:ports/{name}", repo=root)
        if not actual:
            problems.append(
                f"ports/{name}/: not committed, so it has no git-tree to publish",
            )
        elif recorded != actual:
            problems.append(
                f"{versions_path.relative_to(root)}: {version}#{port_version} "
                f"publishes git-tree {recorded}, but ports/{name} at HEAD is "
                f"{actual}. The registry serves the old port.",
            )

    baseline_path = root / "versions" / "baseline.json"
    baseline = json.loads(baseline_path.read_text()).get("default", {})
    entry = baseline.get(name)
    if entry is None:
        problems.append(f"versions/baseline.json: no default version for {name}")
    elif (
        entry.get("baseline") != version or entry.get("port-version", 0) != port_version
    ):
        problems.append(
            f"versions/baseline.json: {name} defaults to "
            f"{entry.get('baseline')}#{entry.get('port-version', 0)}, but "
            f"ports/{name}/vcpkg.json declares {version}#{port_version}",
        )

    return problems


HOW = """\
The versions database is generated, never edited by hand. Commit the port
first -- the recorded git-tree describes what is committed -- then:

    vcpkg --x-builtin-ports-root=./ports \\
          --x-builtin-registry-versions-dir=./versions \\
          x-add-version --all --verbose

Changing a published port without changing its version is refused, because a
consumer who already resolved that version would silently get different code.
Bump `version-semver` for an upstream release, or `port-version` when only the
packaging changed.
"""


def main() -> int:
    """Check every port against the versions database and report."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=pathlib.Path,
        default=pathlib.Path(),
        help="repository root (default: the working directory)",
    )
    parser.add_argument(
        "--how",
        action="store_true",
        help="print how to regenerate the versions database and exit",
    )
    args = parser.parse_args()

    if args.how:
        print(HOW, end="")
        return 0

    root = args.root.resolve()
    ports_root = root / "ports"
    if not ports_root.is_dir():
        print(f"{ports_root}: no ports directory", file=sys.stderr)
        return 2

    problems: list[str] = []

    # `x-add-version --all` reads every entry under `ports/` as a port and
    # reports an error for anything else. Prose belongs beside the registry,
    # not inside it.
    strays = sorted(entry.name for entry in ports_root.iterdir() if not entry.is_dir())
    problems += [
        f"ports/{stray}: not a port directory. `x-add-version --all` reports "
        f"an error for it on every run."
        for stray in strays
    ]

    ports = sorted(entry for entry in ports_root.iterdir() if entry.is_dir())
    if not ports:
        print(f"{ports_root}: no ports", file=sys.stderr)
        return 2

    dirty = git("status", "--porcelain", "--", "ports", repo=root)
    if dirty:
        problems.append(
            "ports/ has uncommitted changes. The published git-tree describes "
            "HEAD, so this check cannot speak for your working tree.",
        )

    for port in ports:
        problems += check_port(root, port)

    if problems:
        for problem in problems:
            print(f"error: {problem}", file=sys.stderr)
        print(f"\n{HOW}", file=sys.stderr)
        return 1

    names = ", ".join(port.name for port in ports)
    print(f"registry consistent: {names}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
