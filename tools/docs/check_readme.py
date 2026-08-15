"""Every C++ example in the README is code that compiles and runs.

A README is the most-read file in a repository and the least-tested. The
snippets in this one are not written in the README at all: they live in
`examples/05-readme.cpp`, which is built and executed against a real tmux by
the same CTest run as everything else, and the README quotes them.

This checks the quoting. Each ``` cpp block in README.md must appear verbatim
as a `#region` in that example, and each region must be quoted somewhere. A
sample that drifts from the code, or code nobody shows, fails the build rather
than greeting the next reader.
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


def quoted(readme: pathlib.Path) -> list[str]:
    """C++ blocks the README shows, in the order it shows them."""
    return [match.group("body").strip() for match in FENCE.finditer(readme.read_text())]


def main() -> int:
    """Check the README against the example, reporting what disagrees."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--readme", type=pathlib.Path, default=pathlib.Path("README.md")
    )
    parser.add_argument(
        "--example",
        type=pathlib.Path,
        default=pathlib.Path("examples/05-readme.cpp"),
    )
    parser.add_argument(
        "--fix",
        action="store_true",
        help="rewrite the README's C++ blocks from the example they quote",
    )
    arguments = parser.parse_args()

    available = regions(arguments.example)
    if not available:
        print(f"{arguments.example}: no #region markers found", file=sys.stderr)
        return 1

    if arguments.fix:
        return _fix(arguments.readme, arguments.example, available)

    shown = quoted(arguments.readme)

    by_body = {body: name for name, body in available.items()}
    used: set[str] = set()
    problems: list[str] = []

    for index, block in enumerate(shown, start=1):
        name = by_body.get(block)
        if name is None:
            near = _closest(block, available)
            problems.append(
                f"{arguments.readme}: C++ block {index} is not any region of "
                f"{arguments.example}{near}"
            )
            continue
        used.add(name)

    problems.extend(
        f"{arguments.example}: region {name!r} is compiled, README never shows it"
        for name in sorted(set(available) - used)
    )

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

    print(f"{len(shown)} C++ blocks, all quoting {arguments.example}")
    return 0


def _fix(readme: pathlib.Path, example: pathlib.Path, available: dict[str, str]) -> int:
    """Rewrite each C++ block from the region it already resembles.

    Matching is by first line, which is what makes this safe to run: a block
    that no longer resembles anything is left alone and reported, rather than
    silently replaced with whichever region happened to come next.
    """
    by_first = {
        body.splitlines()[0]: body for body in available.values() if body.splitlines()
    }
    replaced = 0
    unmatched: list[int] = []
    counter = iter(range(1, 10_000))

    def substitute(match: re.Match[str]) -> str:
        nonlocal replaced
        index = next(counter)
        block = match.group("body").strip()
        first = block.splitlines()[0] if block.splitlines() else ""
        body = by_first.get(first)
        if body is None:
            unmatched.append(index)
            return match.group(0)
        if body != block:
            replaced += 1
        return f"```cpp\n{body}\n```\n"

    updated = FENCE.sub(substitute, readme.read_text())
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
    first = block.splitlines()[0] if block.splitlines() else ""
    for name, body in available.items():
        if body.splitlines() and body.splitlines()[0] == first:
            return f" (closest: {name!r}, which starts the same way)"
    return ""


if __name__ == "__main__":
    raise SystemExit(main())
