"""Every README region compiles on its own, with nothing the reader cannot see.

`check_readme.py` proves the README quotes the example verbatim. That makes the
lines real, not complete: a `#region` starts wherever its marker sits, so every
binding created above it stays in scope for the compiler and vanishes for the
reader. The opening region took its `server` from `ScratchServer`, a helper in
the example's own directory, so the first thing anyone copied could not build.

So each region is compiled here as the whole of a translation unit. A region
that reads a binding it does not create says so on its first line:

    // Given: const libtmux::Server& server

Those become the parameters of the function the body is placed in, which makes
the list exact in both directions: a binding the region uses and the line omits
fails as "not declared in this scope", and one the line names and the region
never touches fails under -Werror=unused-parameter. Because the line is part of
the region, `check_readme.py --fix` carries it into the README, so the reader is
told what the snippet assumes rather than the compiler being the only party that
knows.

Compiling is -fsyntax-only: the point is whether the text a reader pastes is
accepted, and nothing here needs to link or run.

    python3 -m tools.docs.check_example_isolation
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

from tools.docs.check_readme import regions

GIVEN = re.compile(r"^//\s*Given:\s*(?P<declarations>.+?)\s*$")
INCLUDE = re.compile(r'^\s*#include\s+([<"].+?[>"])\s*$', re.MULTILINE)
AUTO_TYPED = re.compile(r"\bauto\b")

# The example's own directory is the harness. A reader has no scratch_server.hpp,
# so a region that needs it is a region that cannot be pasted.
HARNESS_HEADERS = frozenset({'"scratch_server.hpp"'})


def declarations(body: str) -> tuple[list[str], str]:
    """Return the bindings a region declares it is given, and the rest of its body.

    A declaration cannot spell its type as `auto`. `program()` places these as
    the parameters of a function, and an `auto` parameter makes that function
    an unconstrained template - C++20's abbreviated syntax, legal since a
    template needs no instantiation to satisfy `-fsyntax-only`. Every region
    would "compile" then, proving nothing about the type a reader actually
    has.
    """
    lines = body.split("\n")
    match = GIVEN.match(lines[0].strip()) if lines else None
    if match is None:
        return [], body
    given = [
        part.strip() for part in match.group("declarations").split(";") if part.strip()
    ]
    auto_typed = [part for part in given if AUTO_TYPED.search(part)]
    if auto_typed:
        msg = (
            f"a Given line cannot declare 'auto': {auto_typed!r} would make "
            "the region an unconstrained template that -fsyntax-only never "
            "instantiates, so it would pass without checking the body"
        )
        raise SystemExit(msg)
    return given, "\n".join(lines[1:]).lstrip("\n")


def program(includes: list[str], given: list[str], body: str) -> str:
    """One region, as the only code in a translation unit."""
    prologue = "\n".join(f"#include {header}" for header in includes)
    signature = ", ".join(given)
    return (
        f"{prologue}\n\n"
        f"static int region({signature}) {{\n{body}\n  return 0;\n}}\n"
        # Referenced so the function is not merely unused, which some
        # configurations warn about and none of them should here.
        f"\nint main() {{ return 0; }}\n"
    )


def compile_alone(
    source: str, include_dir: pathlib.Path, standard: str, compiler: str
) -> tuple[bool, str]:
    """Syntax-check one synthesized translation unit."""
    with tempfile.TemporaryDirectory() as directory:
        unit = pathlib.Path(directory) / "region.cpp"
        unit.write_text(source)
        completed = subprocess.run(
            [
                compiler,
                f"-std={standard}",
                "-fsyntax-only",
                "-Werror=unused-parameter",
                "-Wunused-parameter",
                "-I",
                str(include_dir),
                str(unit),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
    return completed.returncode == 0, completed.stderr


def main() -> int:
    """Compile every region alone, reporting the ones that cannot be."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--example", type=pathlib.Path, default=pathlib.Path("examples/05-readme.cpp")
    )
    parser.add_argument("--include", type=pathlib.Path, default=pathlib.Path("include"))
    parser.add_argument("--std", default="c++23")
    parser.add_argument("--compiler", default="g++")
    arguments = parser.parse_args()

    available = regions(arguments.example)
    if not available:
        print(f"{arguments.example}: no #region markers found", file=sys.stderr)
        return 1

    includes = [
        header
        for header in INCLUDE.findall(arguments.example.read_text())
        if header not in HARNESS_HEADERS
    ]

    failures = 0
    for name, body in available.items():
        given, without = declarations(body)
        ok, message = compile_alone(
            program(includes, given, without),
            arguments.include,
            arguments.std,
            arguments.compiler,
        )
        if ok:
            continue
        failures += 1
        print(
            f"{arguments.example}: region {name!r} does not compile on its own:\n"
            f"{message.rstrip()}\n"
            "Declare what it is given on the region's first line, as\n"
            "    // Given: const libtmux::Server& server; "
            "const libtmux::Session& session\n",
            file=sys.stderr,
        )

    if failures:
        print(
            f"{failures} of {len(available)} regions cannot be pasted as shown.",
            file=sys.stderr,
        )
        return 1

    print(f"{len(available)} README regions each compile alone")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
