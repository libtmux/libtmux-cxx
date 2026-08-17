"""Checks for the vcpkg git registry this repository serves.

``check`` reads the repository and answers instantly. ``probe`` needs a real
vcpkg and builds a real consumer against it, which is slower and proves more.

Run from the repository root::

    python3 -m tools.vcpkg check
"""

from __future__ import annotations

import argparse
import pathlib

from tools.vcpkg import check, probe, release


def _run_check(args: argparse.Namespace) -> int:
    return check.run(args.root.resolve(), how=args.how)


def _run_release(args: argparse.Namespace) -> int:
    return release.run(
        args.root.resolve(),
        version=args.version,
        sha512=args.sha512,
        port=args.port,
    )


def _run_probe(args: argparse.Namespace) -> int:
    return probe.run(
        args.root.resolve(),
        args.vcpkg.resolve(),
        port=args.port,
        triplet=args.triplet,
        features=args.feature,
        keep=args.keep,
    )


def main() -> int:
    """Dispatch to a registry check."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=pathlib.Path,
        default=pathlib.Path(),
        help="repository root (default: the working directory)",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    consistent = subparsers.add_parser(
        "check",
        help="the versions database still describes the ports beside it",
    )
    consistent.add_argument(
        "--how",
        action="store_true",
        help="print how to regenerate the versions database and exit",
    )
    consistent.set_defaults(handler=_run_check)

    install = subparsers.add_parser(
        "probe",
        help="a consumer resolves the port from this repository and builds",
    )
    install.add_argument(
        "--vcpkg",
        type=pathlib.Path,
        required=True,
        help="a bootstrapped vcpkg checkout",
    )
    install.add_argument("--port", default="libtmux", help="port to resolve")
    install.add_argument("--triplet", default=None, help="vcpkg target triplet")
    install.add_argument(
        "--feature",
        action="append",
        default=[],
        help="port feature to ask for; repeatable",
    )
    install.add_argument(
        "--keep",
        type=pathlib.Path,
        default=None,
        help="write the throwaway consumer here and leave it behind",
    )
    install.set_defaults(handler=_run_probe)

    cut = subparsers.add_parser(
        "release",
        help="point the port at a release whose tag now exists",
    )
    cut.add_argument("--version", required=True, help="version the tag names")
    cut.add_argument(
        "--sha512",
        required=True,
        help="SHA512 of the archive GitHub generates for that tag",
    )
    cut.add_argument("--port", default="libtmux", help="port to update")
    cut.set_defaults(handler=_run_release)

    args = parser.parse_args()
    return int(args.handler(args))


if __name__ == "__main__":
    raise SystemExit(main())
