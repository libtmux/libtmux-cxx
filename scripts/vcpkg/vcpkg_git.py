"""The one git call the registry checks need."""

from __future__ import annotations

import pathlib
import subprocess


def git(*args: str, repo: pathlib.Path) -> str:
    """Run a git command in ``repo`` and return its stripped stdout.

    Returns an empty string when git fails, because every caller here treats
    "no answer" and "git said no" the same way: the tree it asked about is not
    there to publish.
    """
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
