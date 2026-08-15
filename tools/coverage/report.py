"""Measure how much of the shipped surface the tests actually reach.

Source-based coverage, because most of this library is in headers and a
line-table report drops the ones that were only instantiated.

Every instrumented binary has to be named on the command line: `llvm-cov`
reports on the objects it is given, and a single-object run silently omits
everything the other binaries covered.
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys


def instrumented_binaries(build: pathlib.Path) -> list[pathlib.Path]:
    """Return every executable the run produced, in a stable order."""
    found = [
        path
        for path in sorted(build.rglob("*"))
        if path.is_file()
        and path.stat().st_mode & 0o111
        and path.suffix not in {".a", ".so", ".cmake", ".py", ".profraw", ".profdata"}
        and "CMakeFiles" not in path.parts
        and "_deps" not in path.parts
    ]
    return found


def main() -> int:
    """Merge the profiles and print a report over the shipped sources."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=pathlib.Path, required=True)
    parser.add_argument("--source", type=pathlib.Path, required=True)
    parser.add_argument(
        "--floor",
        type=float,
        default=0.0,
        help="fail when total line coverage falls below this percentage",
    )
    arguments = parser.parse_args()

    profiles = sorted((arguments.build / "profiles").glob("*.profraw"))
    if not profiles:
        sys.stderr.write(
            "no profiles: run ctest with LLVM_PROFILE_FILE pointing into "
            f"{arguments.build / 'profiles'}\n"
        )
        return 1

    merged = arguments.build / "all.profdata"
    subprocess.run(
        [
            "llvm-profdata-18",
            "merge",
            "-sparse",
            *map(str, profiles),
            "-o",
            str(merged),
        ],
        check=True,
    )

    binaries = instrumented_binaries(arguments.build)
    if not binaries:
        sys.stderr.write(f"no instrumented binaries under {arguments.build}\n")
        return 1

    # Profiles accumulate, so a run that wrote none leaves the last run's
    # behind and the report answers about code that is no longer there. That
    # reads as "coverage did not change", which is the one conclusion this
    # must never invent.
    newest_profile = max(profile.stat().st_mtime for profile in profiles)
    newest_binary = max(binary.stat().st_mtime for binary in binaries)
    if newest_profile < newest_binary:
        sys.stderr.write(
            "the newest profile predates the newest binary, so this would "
            "report on a build that has since changed. Re-run:\n"
            "  ctest --preset cxx-coverage\n"
        )
        return 1

    objects: list[str] = []
    for binary in binaries:
        objects += ["-object", str(binary)]

    report = subprocess.run(
        [
            "llvm-cov-18",
            "report",
            *objects[1:],  # the first binary is positional
            f"-instr-profile={merged}",
            str(arguments.source / "include" / "libtmux"),
            str(arguments.source / "src"),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    sys.stdout.write(report.stdout)

    total = report.stdout.strip().splitlines()[-1].split()
    # The report's columns are region, function, line, branch; each is a count,
    # a miss count and a percentage.
    line_percentage = float(total[9].rstrip("%"))
    if line_percentage < arguments.floor:
        sys.stderr.write(
            f"line coverage {line_percentage:.2f}% is below the floor "
            f"of {arguments.floor:.2f}%\n"
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
