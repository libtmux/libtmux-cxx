"""What including this library costs a translation unit.

Measured as a ratio against a translation unit that includes only `<string>`,
not as an absolute: the absolute depends on the standard library, and on this
machine the same headers preprocess to 4.5 MB under libc++ and 3.4 MB under
libstdc++. The ratio between the two moves far less, so it is what a budget can
be written against and still mean the same thing in every lane.
"""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile

# The umbrella measures 3.67x under libc++ and 3.85x under libstdc++ today, so
# one budget has to clear the higher of the two and libstdc++ is the lane that
# bites first. Chosen by regression rather than by round number: adding
# `<regex>` and `<locale>` to the umbrella reaches 3.82x under libc++ and 4.24x
# under libstdc++, so 4.0 catches that and 4.5 catches nothing short of a
# catastrophe. Lower it when a real reduction lands.
DEFAULT_BUDGET = 4.0

BASELINE = "#include <string>\n"


def preprocessed_bytes(
    compiler: str, standard: str, include: pathlib.Path, source: str, stdlib: str | None
) -> int:
    """How many bytes `source` preprocesses to, headers and all."""
    with tempfile.TemporaryDirectory() as directory:
        unit = pathlib.Path(directory) / "unit.cpp"
        unit.write_text(source)
        command = [compiler, f"-std={standard}", "-I", str(include), "-E", str(unit)]
        if stdlib:
            command.insert(1, f"-stdlib={stdlib}")
        finished = subprocess.run(command, capture_output=True, check=False)
        if finished.returncode != 0:
            sys.exit(
                finished.stderr.decode(errors="replace").strip()
                or f"{compiler} could not preprocess the unit"
            )
        return len(finished.stdout)


def main(argv: list[str] | None = None) -> int:
    """Report the ratio, and fail under `--check` when it is over budget."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--include", type=pathlib.Path, default=pathlib.Path("include"))
    parser.add_argument("--header", default="libtmux/libtmux.hpp")
    parser.add_argument("--compiler", default=shutil.which("clang++") or "c++")
    parser.add_argument("--standard", default="gnu++23")
    # clang needs telling: its default standard library has no `std::expected`,
    # and the header says so rather than being measured.
    parser.add_argument("--stdlib", default=None)
    parser.add_argument("--budget", type=float, default=DEFAULT_BUDGET)
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args(argv)
    if arguments.stdlib is None and "clang" in pathlib.Path(arguments.compiler).name:
        arguments.stdlib = "libc++"

    baseline = preprocessed_bytes(
        arguments.compiler,
        arguments.standard,
        arguments.include,
        BASELINE,
        arguments.stdlib,
    )
    measured = preprocessed_bytes(
        arguments.compiler,
        arguments.standard,
        arguments.include,
        f'#include "{arguments.header}"\n',
        arguments.stdlib,
    )
    if baseline == 0:
        sys.exit("the baseline unit preprocessed to nothing")
    ratio = measured / baseline
    print(
        f"{arguments.header}: {measured:,} bytes, {ratio:.2f}x a <string> unit "
        f"({baseline:,} bytes); budget {arguments.budget:.2f}x"
    )
    if arguments.check and ratio > arguments.budget:
        print(
            f"over budget: {ratio:.2f}x exceeds {arguments.budget:.2f}x. Including "
            f"this header costs every translation unit that reads it.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
