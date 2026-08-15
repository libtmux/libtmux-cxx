"""Run the mutation catalogue."""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys
import typing as t

from .catalogue import CATALOGUE
from .runner import failed, report, run


def main(argv: t.Sequence[str] | None = None) -> int:
    """Run every catalogued mutation, or one named with ``--id``.

    Parameters
    ----------
    argv : Sequence[str] | None, optional
        Arguments after the module name.

    Returns
    -------
    int
        Zero only when every mutation was killed.

    Examples
    --------
    >>> callable(main)
    True
    """
    parser = argparse.ArgumentParser(prog="python -m tools.mutate")
    parser.add_argument("--preset", default="cxx-dev")
    parser.add_argument("--repository", type=pathlib.Path, default=pathlib.Path())
    parser.add_argument("--id", dest="chosen", action="append")
    parser.add_argument("--build-root", type=pathlib.Path)
    parsed = parser.parse_args(argv)
    chosen = set(parsed.chosen or ())
    wanted = [m for m in CATALOGUE if not chosen or m.mutation_id in chosen]
    if not wanted:
        print("no mutation matched", file=sys.stderr)
        return 2
    # It edits the working tree in place and puts each file back as it goes.
    # Anything that reads the tree meanwhile — a commit, another build — can
    # catch a file mid-mutation, so say so rather than leave it to be found
    # in a diff later.
    print(
        f"editing {len(wanted)} source(s) in place; do not commit or build "
        "from this tree until it finishes",
        file=sys.stderr,
    )
    outcomes = [
        run(mutation, parsed.repository, parsed.preset, build_root=parsed.build_root)
        for mutation in wanted
    ]
    # The last mutation restored its source but left its binary in the tree,
    # so the next `ctest` would run code nobody has any more. Build once on
    # the way out and the tree is honest again.
    subprocess.run(
        ["cmake", "--build", "--preset", parsed.preset],
        cwd=parsed.build_root or parsed.repository,
        capture_output=True,
        check=False,
    )
    print(report(outcomes))
    return int(failed(outcomes))


if __name__ == "__main__":
    raise SystemExit(main())
