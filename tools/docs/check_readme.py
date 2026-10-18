"""Every C++ example in the README is code that compiles and runs.

A README is the most-read file in a repository and the least-tested. The
snippets in this one are not written in the README at all: they live in
`examples/05-readme.cpp`, which is built and executed against a real tmux by
the same CTest run as everything else, and the README quotes them.

This checks the quoting. Each ``` cpp block in a README must appear verbatim
as a `#region` in that example, and each region must be quoted somewhere. A
sample that drifts from the code, or code nobody shows, fails the build rather
than greeting the next reader. The top-level README and the core package README
under `include/libtmux/` both quote it.

A whole program is quoted from its own file instead. A block that follows
`<!-- cpp-file: PATH -->` must equal that file, and one that follows
`<!-- text-file: PATH -->` must equal the file holding what the program prints,
which its test compares against the real output.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
import textwrap

REGION = re.compile(
    r"^[ \t]*// #region (?P<name>\S+)\n(?P<body>.*?)^[ \t]*// #endregion (?P=name)\n",
    re.MULTILINE | re.DOTALL,
)
FENCE = re.compile(r"^```cpp\n(?P<body>.*?)^```\n", re.MULTILINE | re.DOTALL)
FILE_BLOCK = re.compile(
    r"<!-- (?P<kind>cpp|text)-file: (?P<path>\S+) -->\n\n```(?P=kind)\n"
    r"(?P<body>.*?)^```\n",
    re.MULTILINE | re.DOTALL,
)
GIVEN = re.compile(r"^// Given: .+$")
INCLUDE = re.compile(r"^#include [<\"].+[>\"]$")


def _matching_line(body: str) -> str:
    """Return the line `_fix` correlates a block by: the first one that is not `Given:`.

    A region declares what it assumes on its very first line (see
    `check_example_isolation.py`), so matching on line one verbatim would
    key every such region on the same literal text. The line after it is
    still the one description unique to that region.
    """
    lines = body.splitlines()
    if lines and GIVEN.match(lines[0]):
        lines = lines[1:]
    return lines[0] if lines else ""


def include_lines(block: str) -> list[str] | None:
    """Return the directives of a block that is only `#include` lines, else None.

    Such a block shows which header to include, not a statement to run, so
    there is no region for it. It is held to the example instead: every line
    must be one the example itself includes.
    """
    lines = [line.strip() for line in block.splitlines() if line.strip()]
    if lines and all(INCLUDE.match(line) for line in lines):
        return lines
    return None


def regions(source: pathlib.Path) -> dict[str, str]:
    """Named snippets defined in the example, dedented to column zero."""
    found: dict[str, str] = {}
    for match in REGION.finditer(source.read_text()):
        name = match.group("name")
        if name in found:
            msg = f"{source}: two regions named {name}"
            raise SystemExit(msg)
        found[name] = textwrap.dedent(match.group("body")).strip()
    return found


def _file_spans(text: str) -> list[tuple[int, int]]:
    """Return where each block quoted from a whole file starts and ends."""
    return [(m.start(), m.end()) for m in FILE_BLOCK.finditer(text)]


def _inside(spans: list[tuple[int, int]], position: int) -> bool:
    """Report whether a position falls inside one of the spans."""
    return any(start <= position < end for start, end in spans)


def quoted(readme: pathlib.Path) -> list[str]:
    """C++ blocks quoted from regions, in the order the README shows them."""
    text = readme.read_text()
    spans = _file_spans(text)
    return [
        match.group("body").strip()
        for match in FENCE.finditer(text)
        if not _inside(spans, match.start())
    ]


def file_problems(readme: pathlib.Path, root: pathlib.Path) -> tuple[list[str], int]:
    """Compare each block quoted from a whole file to that file."""
    problems: list[str] = []
    found = list(FILE_BLOCK.finditer(readme.read_text()))
    for match in found:
        source = root / match.group("path")
        if not source.is_file():
            problems.append(f"{readme}: {match.group('path')} does not exist")
        elif match.group("body").strip() != source.read_text().strip():
            problems.append(
                f"{readme}: the block after {match.group('kind')}-file: "
                f"{match.group('path')} is not that file; re-run with --fix"
            )
    return problems, len(found)


def check(
    readmes: list[pathlib.Path],
    example: pathlib.Path,
    available: dict[str, str],
    includes: frozenset[str],
    root: pathlib.Path = pathlib.Path(),
) -> tuple[list[str], int]:
    """Compare every README's C++ blocks to the regions, and return what disagrees.

    A region may be quoted by any of the READMEs, so one that only a package
    README shows is not stale. A block that matches no region is a problem in
    whichever README holds it.
    """
    by_body = {body: name for name, body in available.items()}
    used: set[str] = set()
    problems: list[str] = []
    blocks = 0
    for readme in readmes:
        wholes, count = file_problems(readme, root)
        problems.extend(wholes)
        blocks += count
        for index, block in enumerate(quoted(readme), start=1):
            blocks += 1
            directives = include_lines(block)
            if directives is not None:
                problems.extend(
                    f"{readme}: C++ block {index} includes {line!r}, which "
                    f"{example} does not"
                    for line in directives
                    if line not in includes
                )
                continue
            name = by_body.get(block)
            if name is None:
                near = _closest(block, available)
                problems.append(
                    f"{readme}: C++ block {index} is not any region of {example}{near}"
                )
                continue
            used.add(name)

    problems.extend(
        f"{example}: region {name!r} is compiled, no README shows it"
        for name in sorted(set(available) - used)
    )
    return problems, blocks


def main() -> int:
    """Check the READMEs against the example, reporting what disagrees."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--readme",
        type=pathlib.Path,
        action="append",
        dest="readmes",
        help="a README that quotes the example; repeat for more than one "
        "(default: README.md)",
    )
    parser.add_argument(
        "--example",
        type=pathlib.Path,
        default=pathlib.Path("examples/05-readme.cpp"),
    )
    parser.add_argument(
        "--fix",
        action="store_true",
        help="rewrite each README's C++ blocks from the example they quote",
    )
    arguments = parser.parse_args()
    readmes = arguments.readmes or [pathlib.Path("README.md")]

    available = regions(arguments.example)
    if not available:
        print(f"{arguments.example}: no #region markers found", file=sys.stderr)
        return 1

    if arguments.fix:
        return max(_fix(readme, arguments.example, available) for readme in readmes)

    includes = frozenset(
        line.strip()
        for line in arguments.example.read_text().splitlines()
        if INCLUDE.match(line.strip())
    )
    problems, blocks = check(readmes, arguments.example, available, includes)

    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        print(
            "\nThe README quotes code that is built and run. Edit "
            f"{arguments.example}, then re-run with --fix to bring the README "
            "across. Editing the README alone is what this check is for.",
            file=sys.stderr,
        )
        return 1

    print(f"{blocks} C++ blocks, all quoting {arguments.example}")
    return 0


def _fix(
    readme: pathlib.Path,
    example: pathlib.Path,
    available: dict[str, str],
    root: pathlib.Path = pathlib.Path(),
) -> int:
    """Rewrite each C++ block from the region it already resembles.

    Matching is by first line, which is what makes this safe to run: a block
    that no longer resembles anything is left alone and reported, rather than
    silently replaced with whichever region happened to come next.
    """
    by_first = {_matching_line(body): body for body in available.values()}
    replaced = 0
    unmatched: list[int] = []
    counter = iter(range(1, 10_000))

    def whole(match: re.Match[str]) -> str:
        nonlocal replaced
        source = root / match.group("path")
        if not source.is_file():
            return match.group(0)
        body = source.read_text().strip()
        if body != match.group("body").strip():
            replaced += 1
        kind = match.group("kind")
        return (
            f"<!-- {kind}-file: {match.group('path')} -->\n\n```{kind}\n{body}\n```\n"
        )

    text = FILE_BLOCK.sub(whole, readme.read_text())
    spans = _file_spans(text)

    def substitute(match: re.Match[str]) -> str:
        nonlocal replaced
        if _inside(spans, match.start()):
            return match.group(0)
        index = next(counter)
        block = match.group("body").strip()
        if include_lines(block) is not None:
            return match.group(0)
        first = _matching_line(block)
        body = by_first.get(first)
        if body is None:
            unmatched.append(index)
            return match.group(0)
        if body != block:
            replaced += 1
        return f"```cpp\n{body}\n```\n"

    updated = FENCE.sub(substitute, text)
    readme.write_text(updated)

    for index in unmatched:
        print(
            f"{readme}: C++ block {index} resembles no region of {example}; "
            f"left as it was",
            file=sys.stderr,
        )
    print(f"{readme}: {replaced} block(s) brought across from {example}")
    return 1 if unmatched else 0


def _closest(block: str, available: dict[str, str]) -> str:
    """Name the region a block was probably meant to be, to save a diff hunt."""
    first = _matching_line(block)
    for name, body in available.items():
        if _matching_line(body) == first:
            return f" (closest: {name!r}, which starts the same way)"
    return ""


if __name__ == "__main__":
    raise SystemExit(main())
