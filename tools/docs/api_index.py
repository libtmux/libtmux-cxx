"""Render the public C++ surface as one browsable page.

The headers are the source of truth. This walks them, keeps the prose already
written above each declaration, and lays the result out by type — so the
reference cannot drift from the code, and nobody has to restate a signature in
two places.

Run with ``--check`` to fail when the committed page is out of date.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
import typing as t

HEADER_ORDER = [
    # First, because it is what a reader includes first. Its absence from this
    # list is what the completeness check in `render` found the moment it
    # existed: the one header everybody uses had never been in the reference.
    "libtmux.hpp",
    "server.hpp",
    "entities.hpp",
    "snapshot.hpp",
    "filter_expr.hpp",
    "relations.hpp",
    "cardinality.hpp",
    "command.hpp",
    "options.hpp",
    "control.hpp",
    "batch.hpp",
    "chain.hpp",
    "keys.hpp",
    "capture.hpp",
    "target.hpp",
    "socket.hpp",
    "format.hpp",
    "version.hpp",
    "lowering.hpp",
    "lowered_node.hpp",
    "legacy_lookup.hpp",
    "expected.hpp",
    "abi.hpp",
]

# `libtmux::testing` gets a page of its own rather than a section at the end of
# the library's. A reference is read by someone deciding what to call, and the
# first thing they should meet is the library — not the scaffolding for testing
# against it, which is a separate target they have to ask for by name.
TESTING_HEADER_ORDER = [
    "scoped_server.hpp",
    "capabilities.hpp",
    "tmux_version.hpp",
    "environment_guard.hpp",
]


class Page(t.NamedTuple):
    """One generated reference page and the headers it covers."""

    title: str
    preamble: list[str]
    headers: list[str]
    include_prefix: str
    order_name: str
    output: str


PAGES = {
    "library": Page(
        title="API reference",
        preamble=[],
        headers=HEADER_ORDER,
        include_prefix="libtmux/",
        order_name="HEADER_ORDER",
        output="docs/api.md",
    ),
    "testing": Page(
        title="Testing API reference",
        preamble=[
            "`libtmux::testing` — the private-tmux-server fixture this project's own",
            "suite runs on, shipped so a consumer's suite can run on it too. Ask for",
            "it with `find_package(libtmux COMPONENTS testing)`; it is not part of",
            "the library, and a program that only uses libtmux links none of it.",
            "",
        ],
        headers=TESTING_HEADER_ORDER,
        include_prefix="libtmux/testing/",
        order_name="TESTING_HEADER_ORDER",
        output="docs/api-testing.md",
    ),
}

DECLARATION = re.compile(
    r"^\s*(?:\[\[nodiscard\]\]\s*)?"
    r"(?:static\s+|virtual\s+|inline\s+|constexpr\s+|explicit\s+)*"
    # Braces are allowed inside the parentheses: a default argument
    # like `std::chrono::seconds{5}` is part of the signature, and
    # excluding it dropped the declaration from the reference
    # entirely. The terminating `;` still bounds the match.
    r"(?P<rest>[A-Za-z_~][\w:<>,&*\s]*\((?:[^();]|\([^()]*\))*\)[^;{]*)[;{]"
)
# A definition, not a forward declaration: the brace is what opens a type.
TYPE = re.compile(r"^(?P<kind>class|struct|enum class)\s+(?P<name>\w+)[^;]*\{")
DETAIL = re.compile(r"^namespace detail\s*\{")


class Entry(t.NamedTuple):
    """One documented declaration.

    Attributes
    ----------
    signature : str
        The declaration as written, collapsed onto one line.
    prose : list[str]
        The comment lines immediately above it, markers stripped.
    """

    signature: str
    prose: list[str]


def _collapse(text: str) -> str:
    """Put a declaration that spans lines back onto one."""
    return re.sub(r"\s+", " ", text).strip()


def _prose_above(lines: list[str], index: int) -> list[str]:
    """Return the comment block directly above ``index``, markers stripped."""
    collected: list[str] = []
    cursor = index - 1
    while cursor >= 0:
        stripped = lines[cursor].strip()
        if not stripped.startswith("//"):
            break
        collected.append(stripped.removeprefix("//").strip())
        cursor -= 1
    return list(reversed(collected))


def read_header(path: pathlib.Path) -> tuple[list[str], dict[str, list[Entry]]]:
    """Return a header's opening prose and its declarations, grouped by type."""
    lines = path.read_text(encoding="utf-8").splitlines()

    overview: list[str] = []
    for line in lines[1:]:
        stripped = line.strip()
        if stripped.startswith("//"):
            overview.append(stripped.removeprefix("//").strip())
        elif overview:
            break

    grouped: dict[str, list[Entry]] = {}
    current = ""
    depth = 0
    detail_depth = -1
    consumed_through = -1
    public = True
    for index, line in enumerate(lines):
        stripped = line.strip()

        # `detail` is private by convention; a reference that lists it invites
        # someone to call it.
        if DETAIL.match(stripped):
            detail_depth = 0
        if detail_depth >= 0:
            detail_depth += line.count("{") - line.count("}")
            if detail_depth <= 0:
                detail_depth = -1
            continue

        if match := TYPE.match(stripped):
            current = match.group("name")
            grouped.setdefault(current, [])
            depth = 0
            # A class starts private, a struct public.
            public = match.group("kind") != "class"
        if stripped.startswith(("public:", "private:", "protected:")):
            public = stripped.startswith("public:")
            continue
        depth += line.count("{") - line.count("}")
        if current and depth <= 0 and stripped.startswith("}"):
            current = ""
            public = True
        if stripped.startswith(("//", "*", "#")) or (
            "operator" in stripped and "(" not in stripped
        ):
            continue

        if index <= consumed_through:
            continue
        # A reference lists what a caller may call.
        if current and not public:
            continue

        window = " ".join(lines[index : index + 4])
        if not (match := DECLARATION.match(window)):
            continue
        signature = _collapse(match.group(0).rstrip("{;"))
        if signature.startswith(("if", "for", "while", "switch", "return", "}")):
            continue
        # A reference lists what a caller may call. Anything that names the
        # private transport, or reaches the reader through an access specifier
        # on the same line, is not that — and a regex over C++ will not always
        # have tracked which side of the specifier it is on.
        if any(
            hidden in signature
            for hidden in ("private:", "protected:", "friend ", "detail::")
        ):
            continue

        # A declaration that wraps spans several lines; the continuations are
        # part of it, not declarations of their own.
        matched = match.group(0)
        spanned = index
        consumed = len(line.strip())
        while consumed < len(_collapse(matched)) and spanned + 1 < len(lines):
            spanned += 1
            consumed += len(lines[spanned].strip()) + 1
        consumed_through = spanned

        grouped.setdefault(current or "(free functions)", []).append(
            Entry(signature=signature, prose=_prose_above(lines, index))
        )
    return overview, grouped


def render(root: pathlib.Path, page: Page) -> str:
    """Render one page's headers, and refuse to leave any of them out."""
    # The order list decides order, not membership. It used to decide both, by
    # skipping anything it did not name — so a new public header was absent
    # from the reference and `--check` stayed green, which is the one thing a
    # documentation gate must never do. Membership now comes from the
    # directory, and a header nobody has placed is an error naming itself.
    present = {path.name for path in root.glob("*.hpp")}
    unplaced = sorted(present - set(page.headers))
    if unplaced:
        listed = ", ".join(unplaced)
        msg = (
            f"{root} has public headers missing from {page.order_name} in "
            f"tools/docs/api_index.py: {listed}. Add them where they should "
            f"read, then regenerate {page.output}."
        )
        raise SystemExit(msg)

    out: list[str] = [
        f"# {page.title}",
        "",
        *page.preamble,
        "Generated from the headers by `tools/docs/api_index.py`; the prose here",
        "is the prose there. Run it with `--check` to prove this page is current.",
        "",
    ]
    for name in page.headers:
        path = root / name
        if not path.exists():
            continue
        overview, grouped = read_header(path)
        out.append(f"## `{page.include_prefix}{name}`")
        out.append("")
        if overview:
            out.extend([" ".join(overview).strip(), ""])
        for type_name, entries in grouped.items():
            if not entries:
                continue
            out.append(
                f"### {type_name}"
                if type_name != "(free functions)"
                else "### Free functions"
            )
            out.append("")
            for entry in entries:
                out.append(f"```cpp\n{entry.signature};\n```")
                if entry.prose:
                    out.append(" ".join(entry.prose).strip())
                out.append("")
    return "\n".join(out).rstrip() + "\n"


def main() -> int:
    """Write or check the reference page."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--include", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--page", choices=sorted(PAGES), default="library")
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()

    rendered = render(arguments.include, PAGES[arguments.page])
    if arguments.check:
        current = (
            arguments.output.read_text(encoding="utf-8")
            if arguments.output.exists()
            else ""
        )
        if current != rendered:
            sys.stderr.write(
                f"{arguments.output} is out of date; regenerate it without --check\n"
            )
            return 1
        return 0
    arguments.output.write_text(rendered, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
