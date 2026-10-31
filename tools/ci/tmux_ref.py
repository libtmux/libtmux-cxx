"""Resolve a tmux release, tag, branch or commit, and build tmux from it.

``resolve`` turns what a person typed into the 40-hex commit it names, which is
what the build cache is keyed on: a key of ``tmux-master`` would freeze a lane
at whatever the first run happened to build. ``build`` fetches exactly that
commit, so a release, a branch tip and a bare commit all take the same path.

Run from the repository root::

    python3 -m tools.ci.tmux_ref resolve 3.8
    python3 -m tools.ci.tmux_ref build <commit> --prefix "$HOME/tmux-builds/tmux"
"""

from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import sys
import tempfile

from tools.ci.plan import REF_PATTERN, SHA_PATTERN, PlanError

REMOTE = "https://github.com/tmux/tmux.git"


def _lines(remote: str, patterns: list[str]) -> dict[str, str]:
    done = subprocess.run(
        ["git", "ls-remote", remote, *patterns],
        capture_output=True,
        text=True,
        check=False,
    )
    if done.returncode != 0:
        message = f"git ls-remote {remote} failed: {done.stderr.strip()}"
        raise PlanError(message)
    found = {}
    for line in done.stdout.splitlines():
        sha, _, name = line.partition("\t")
        found[name] = sha
    return found


def resolve(ref: str, remote: str = REMOTE) -> str:
    """Return the 40-hex commit that ``ref`` names on ``remote``.

    A full commit names itself. Otherwise a tag beats a branch of the same
    name, and an annotated tag resolves to the commit it points at.
    """
    if REF_PATTERN.fullmatch(ref) is None or ref.startswith("-"):
        message = f"tmux ref {ref!r} must match [A-Za-z0-9._/-]{{1,64}}"
        raise PlanError(message)
    if SHA_PATTERN.fullmatch(ref):
        return ref
    found = _lines(
        remote, [f"refs/tags/{ref}", f"refs/tags/{ref}^{{}}", f"refs/heads/{ref}"]
    )
    for name in (f"refs/tags/{ref}^{{}}", f"refs/tags/{ref}", f"refs/heads/{ref}"):
        if name in found:
            return found[name]
    message = (
        f"{remote} has no tag or branch named {ref!r}; "
        "name a commit by its full 40-hex id"
    )
    raise PlanError(message)


def _git(source: pathlib.Path, *args: str) -> None:
    subprocess.run(
        ["git", "-c", "advice.detachedHead=false", "-C", str(source), *args],
        check=True,
    )


def build(
    sha: str,
    prefix: pathlib.Path,
    source: pathlib.Path,
    remote: str = REMOTE,
    jobs: int | None = None,
) -> None:
    """Build and install the tmux at commit ``sha`` under ``prefix``."""
    if SHA_PATTERN.fullmatch(sha) is None:
        message = f"build takes a resolved 40-hex commit, not {sha!r}"
        raise PlanError(message)
    source.mkdir(parents=True, exist_ok=True)
    _git(source, "init", "-q")
    _git(source, "remote", "add", "origin", remote)
    _git(source, "fetch", "-q", "--depth", "1", "origin", sha)
    _git(source, "checkout", "-q", "--detach", "FETCH_HEAD")
    head = subprocess.run(
        ["git", "-C", str(source), "rev-parse", "HEAD"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    if head != sha:
        message = f"fetched {head} when asked for {sha}"
        raise PlanError(message)
    workers = str(jobs or os.cpu_count() or 2)
    for command in (
        ["sh", "autogen.sh"],
        ["./configure", f"--prefix={prefix}"],
        ["make", "-j", workers],
        ["make", "install"],
    ):
        subprocess.run(command, cwd=source, check=True)


def _run_resolve(args: argparse.Namespace) -> int:
    sha = resolve(args.ref, args.remote)
    print(sha)
    if args.github_output:
        with pathlib.Path(args.github_output).open("a") as output:
            output.write(f"sha={sha}\n")
    return 0


def _run_build(args: argparse.Namespace) -> int:
    source = args.source or pathlib.Path(tempfile.gettempdir()) / f"tmux-src-{args.sha}"
    build(args.sha, args.prefix, source, args.remote, args.jobs)
    return 0


def main(argv: list[str] | None = None) -> int:
    """Resolve a tmux ref, or build tmux from a resolved commit."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--remote", default=REMOTE)
    subparsers = parser.add_subparsers(dest="command", required=True)

    resolving = subparsers.add_parser("resolve", help="print the commit a ref names")
    resolving.add_argument("ref")
    resolving.add_argument("--github-output", default="")
    resolving.set_defaults(handler=_run_resolve)

    building = subparsers.add_parser("build", help="build tmux at a commit")
    building.add_argument("sha")
    building.add_argument("--prefix", type=pathlib.Path, required=True)
    building.add_argument("--source", type=pathlib.Path)
    building.add_argument("--jobs", type=int)
    building.set_defaults(handler=_run_build)

    args = parser.parse_args(argv)
    try:
        return args.handler(args)
    except PlanError as error:
        print(f"::error::{error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
