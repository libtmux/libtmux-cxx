"""Every package keeps a changelog of its own, and installs it.

The library and its two programs are released together under one version, but a
user of `libtmux-mcp-server` or `tmux-workspace` should not have to read the
library's history to find what changed in theirs. Each keeps a `CHANGELOG.md`,
and nothing noticed one going missing, so this holds each claim that can be
answered from a checkout:

- the file exists;
- it opens with the name of the package it belongs to, so another package's file
  pasted over it fails where it was pasted;
- it has an `Unreleased` section for the next release to land under;
- it lists the same releases as the root file, since the packages share one
  version; and
- for a program, its CMake install rules put the file under `share/doc`, in a
  directory named for the program.

    python3 -m tools.docs.check_changelogs
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]

# Each package's changelog, and for a program the CMake file whose install rules
# must ship it. The library's is the root file, which also lists what spans the
# packages, and its install rules are not this check's concern.
PACKAGES = {
    "libtmux": ("CHANGELOG.md", None),
    "libtmux-mcp-server": ("apps/mcp/CHANGELOG.md", "apps/mcp/CMakeLists.txt"),
    "tmux-workspace": ("apps/workspace/CHANGELOG.md", "apps/workspace/CMakeLists.txt"),
}

OPENING = re.compile(r"\A# Changelog\s+Notable changes to `([^`]+)`\.$", re.MULTILINE)
RELEASE = re.compile(r"^## (\S.*)$")
FENCE = re.compile(r"^\s*(`{3,}|~{3,})(.*)$")


def outside_fences(markdown: str) -> list[str]:
    """Return the lines of `markdown` that are not inside a code fence.

    A fence closes on a line of the same character, at least as long, with no
    info string, so a shorter or different fence inside one does not end it.
    """
    kept: list[str] = []
    fence: str | None = None
    for line in markdown.splitlines():
        match = FENCE.match(line)
        if fence is None:
            if match:
                fence = match.group(1)
            else:
                kept.append(line)
        elif (
            match
            and match.group(1)[0] == fence[0]
            and len(match.group(1)) >= len(fence)
            and not match.group(2).strip()
        ):
            fence = None
    return kept


def releases(markdown: str) -> list[str]:
    """Return the release headings of a changelog, in order, outside fences."""
    return [
        heading.group(1)
        for line in outside_fences(markdown)
        if (heading := RELEASE.match(line))
    ]


def installs(cmake: str, package: str) -> bool:
    """Say whether the CMake text installs the program's own changelog.

    The rule has to install `CHANGELOG.md` from the program's own directory into
    `doc/<package>` under the data root, outside a comment: a rule that installs
    another file, or another package's, or lands elsewhere, does not count.
    """
    code = "\n".join(re.sub(r"#.*$", "", line) for line in cmake.splitlines())
    return bool(
        re.search(
            r"install\s*\(\s*FILES\s+"
            r'"\$\{CMAKE_CURRENT_SOURCE_DIR\}/CHANGELOG\.md"\s+'
            r'DESTINATION\s+"\$\{CMAKE_INSTALL_DATAROOTDIR\}/doc/'
            rf'{re.escape(package)}"',
            code,
            re.IGNORECASE,
        )
    )


def failures(root: pathlib.Path) -> list[str]:
    """Return every way the packages' changelogs fall short of the policy above."""
    found: list[str] = []
    texts: dict[str, str] = {}
    for name, (path, cmake) in PACKAGES.items():
        file = root / path
        if not file.is_file():
            found.append(f"{path}: {name} has no changelog")
            continue
        text = file.read_text(encoding="utf-8")
        texts[name] = text
        opening = OPENING.search(text)
        if opening is None:
            found.append(f'{path}: does not open with "Notable changes to `{name}`."')
        elif opening.group(1) != name:
            found.append(f"{path}: is the changelog of {opening.group(1)}, not {name}")
        if "## Unreleased" not in outside_fences(text):
            found.append(f'{path}: has no "## Unreleased" section')
        if cmake is not None:
            rules = root / cmake
            if not rules.is_file() or not installs(
                rules.read_text(encoding="utf-8"), name
            ):
                found.append(f"{cmake}: does not install {path} under share/doc/{name}")
    reference = texts.get("libtmux")
    if reference is not None:
        for name, text in texts.items():
            if name != "libtmux" and releases(text) != releases(reference):
                found.append(
                    f"{PACKAGES[name][0]}: lists different releases than CHANGELOG.md; "
                    "the packages share one version, so every file has every release"
                )
    return found


def main() -> int:
    """Report every failure, and exit nonzero if there is one."""
    argparse.ArgumentParser(description=__doc__).parse_args()
    problems = failures(ROOT)
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        return 1
    print(f"every package keeps and installs a changelog: {', '.join(PACKAGES)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
