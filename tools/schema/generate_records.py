"""Generate MCP entity records with GCC 16 reflection, or check committed digests."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
GENERATOR = ROOT / "tools/schema/generate_records.cpp"
OUTPUT = ROOT / "apps/mcp/src/output_records.generated.hpp"
STAMP = OUTPUT.with_suffix(".json")
INPUTS = (GENERATOR, ROOT / ".clang-format", pathlib.Path(__file__).resolve())


def digests() -> dict[str, str]:
    """Hash the generation inputs and checked-in output."""
    return {
        str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in (*INPUTS, OUTPUT)
    }


def main() -> int:
    """Check digests, or compile, execute and format the maintainer generator."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="exit 1 on stale output")
    parser.add_argument(
        "--compiler", help="GCC 16 compiler with <meta> and -freflection"
    )
    parser.add_argument(
        "--clang-format", default="clang-format-18", help="clang-format 18 executable"
    )
    arguments = parser.parse_args()
    if not arguments.compiler:
        if not arguments.check:
            parser.error(
                "regeneration requires --compiler; use --check for digest verification"
            )
        if not STAMP.exists() or json.loads(STAMP.read_text()) != digests():
            sys.stderr.write(
                "MCP entity records are stale; regenerate with --compiler g++-16\n"
            )
            return 1
        print("MCP entity record digests match (no compiler execution)")
        return 0

    version = subprocess.check_output([arguments.clang_format, "--version"], text=True)
    if "version 18." not in version:
        parser.error("generation requires clang-format 18")
    with tempfile.TemporaryDirectory(prefix="libtmux-wire-") as temporary:
        executable = pathlib.Path(temporary) / "generate-records"
        subprocess.run(
            [
                arguments.compiler,
                "-std=c++26",
                "-freflection",
                "-Wall",
                "-Wextra",
                "-Wpedantic",
                "-Werror",
                str(GENERATOR),
                "-o",
                str(executable),
            ],
            check=True,
        )
        raw = subprocess.check_output([str(executable)])
        formatted = subprocess.check_output(
            [arguments.clang_format, "--style=file", f"--assume-filename={OUTPUT}"],
            input=raw,
        )
    if arguments.check:
        if (
            OUTPUT.read_bytes() != formatted
            or not STAMP.exists()
            or json.loads(STAMP.read_text()) != digests()
        ):
            sys.stderr.write(
                "MCP entity records are stale; regenerate without --check\n"
            )
            return 1
        print("MCP entity records match compiler output")
        return 0
    OUTPUT.write_bytes(formatted)
    STAMP.write_text(json.dumps(digests(), indent=2) + "\n")
    print("MCP entity records and digests regenerated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
