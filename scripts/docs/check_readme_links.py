"""Every link in a README resolves, and an API is linked where it is introduced.

A README that sends a reader to a directory, a header or a reference page that
is not there fails them at the moment they trust it most. This checks the part
that can be checked from a checkout:

- a relative link, or a `github.com/libtmux/libtmux-cxx/{tree,blob}/master/`
  link, names a path that exists, and its `#fragment` names a heading;
- the first prose mention of a public type in each section is a link, and the
  reference page for each one the prose names is linked at least once;
- no paragraph or list item renders past 240 characters, so a page stays
  scannable;
- a tool name in a table row or list item of the MCP README links to its page
  at `libtmux.org/en/cxx/latest/mcp/tools/<name>/`;
- each package README opens with the shared logo header, in a centred `div`,
  over art that exists.

Public symbols come from the generated API pages, so a new declaration joins
the rule without anyone editing a list here. A page for a symbol lives at
`libtmux.org/en/cxx/latest/reference/libtmux-<name>/`; `--online` fetches every
external link to prove the page exists, which a checkout cannot.

    python3 tools/docs/check_readme_links.py
    python3 tools/docs/check_readme_links.py --online
"""

from __future__ import annotations

import argparse
import bisect
import concurrent.futures
import dataclasses
import pathlib
import re
import subprocess
import sys
import urllib.error
import urllib.request
from collections.abc import Callable, Iterable

SITE = "https://libtmux.org/en/cxx/latest/"
REFERENCE = SITE + "reference/"
REPOSITORY = "https://github.com/libtmux/libtmux-cxx"

# The READMEs held to the first-mention rule. The rest are checked for links
# that resolve, because a directory index names headers rather than teaching
# them.
LINKED_READMES = (
    "README.md",
    "include/libtmux/README.md",
    "apps/workspace/README.md",
    "apps/mcp/README.md",
    "apps/matrix/README.md",
)

# The package READMEs that open with the logo header. The directories under
# `apps/` that are not packages, `src/`, `cmake/` and the like are internal and
# carry none.
LOGO_READMES = (
    "README.md",
    "include/libtmux/README.md",
    "apps/mcp/README.md",
    "apps/workspace/README.md",
)
LOGO_BEGIN = "<!-- libtmux-logo -->"
LOGO_END = "<!-- /libtmux-logo -->"
LOGO_BLOCK = re.compile(
    r'\A\s*<div align="center">\s*<picture>\s*'
    r'<source srcset="(?P<svg>[^"]+\.svg)" type="image/svg\+xml">\s*'
    r'<img src="(?P<png>[^"]+\.png)" width="128" height="128" '
    r'alt="(?P<alt>libtmux for C\+\+[^"]*)">\s*'
    r"</picture>\s*</div>\s*\Z"
)

# The API pages the symbols come from, and the namespace each one documents.
API_PAGES = (("docs/api.md", ""), ("docs/api-testing.md", "test"))

SYMBOL = re.compile(r"^- \[`(?P<name>[A-Za-z_]\w*)`\]\(#", re.MULTILINE)
CODE_SPAN = re.compile(r"`(?P<code>[^`\n]+)`")
INLINE_LINK = re.compile(
    r"\[(?P<text>(?:[^\[\]]|\[[^\]]*\])*?)\]\((?P<target>[^)\s]+)(?:\s+\"[^\"]*\")?\)"
)
REFERENCE_LINK = re.compile(
    r"\[(?P<text>(?:[^\[\]]|\[[^\]]*\])*?)\]\[(?P<label>[^\]]*)\]"
)
DEFINITION = re.compile(r"^\[(?P<label>[^\]]+)\]:\s*(?P<target>\S+)", re.MULTILINE)
HEADING = re.compile(r"^(?P<marks>#{1,6})\s+(?P<text>.+?)\s*#*\s*$")
TOOL_DEFINITION = re.compile(
    r"^\[tool-(?P<name>[a-z_]+)\]:\s*(?P<target>\S+)", re.MULTILINE
)
SECTION = re.compile(r"^#{1,3} ", re.MULTILINE)
LIST_ITEM = re.compile(r"^(?:[-*] |\d+\. )")
DEFINITION_LINE = re.compile(r"^\[[^\]]+\]:\s")
WALL_CHARACTERS = 240
HTML_ANCHOR = re.compile(r"<a\s+(?:id|name)=\"(?P<name>[^\"]+)\"")
LEADING_NAME = re.compile(r"^(?:libtmux::)?(?:test::)?(?P<name>[A-Za-z_]\w*)")


@dataclasses.dataclass(frozen=True)
class Link:
    """A markdown link: where its text sits, and what it points at."""

    start: int
    end: int
    text: str
    target: str


@dataclasses.dataclass(frozen=True)
class Problem:
    """One thing a README gets wrong, phrased for whoever has to fix it."""

    readme: pathlib.Path
    message: str

    def __str__(self) -> str:
        """Name the file first, as a compiler does."""
        return f"{self.readme}: {self.message}"


def symbols(root: pathlib.Path) -> dict[str, str]:
    """Map each public symbol to the slug of its reference page."""
    found: dict[str, str] = {}
    for page, namespace in API_PAGES:
        path = root / page
        if not path.is_file():
            continue
        prefix = f"libtmux-{namespace}-" if namespace else "libtmux-"
        for match in SYMBOL.finditer(path.read_text(encoding="utf-8")):
            name = match.group("name")
            found.setdefault(name, prefix + name.lower())
    return found


def split_fences(text: str) -> tuple[str, str]:
    """Return the text without code fences, and that again without headings and tables.

    Both keep every offset, so a match in one locates the same character in the
    other. Headings and table rows are not prose: a name there labels or
    indexes something, and the sentence that introduces it comes elsewhere.
    """
    inside = False
    fence = ""
    unfenced: list[str] = []
    prose: list[str] = []
    for line in text.split("\n"):
        stripped = line.lstrip()
        marker = stripped[:3]
        if marker in {"```", "~~~"}:
            if not inside:
                inside, fence = True, marker
            elif marker == fence:
                inside = False
            unfenced.append(" " * len(line))
            prose.append(" " * len(line))
            continue
        if inside:
            unfenced.append(" " * len(line))
            prose.append(" " * len(line))
            continue
        unfenced.append(line)
        indexes = stripped.startswith(("|", "#"))
        prose.append(" " * len(line) if indexes else line)
    return "\n".join(unfenced), "\n".join(prose)


def links_in(text: str) -> list[Link]:
    """Every inline and reference-style link in already-unfenced text."""
    definitions = {
        match.group("label").lower(): match.group("target")
        for match in DEFINITION.finditer(text)
    }
    found = [
        Link(match.start(), match.end(), match.group("text"), match.group("target"))
        for match in INLINE_LINK.finditer(text)
    ]
    for match in REFERENCE_LINK.finditer(text):
        label = (match.group("label") or match.group("text")).strip("` ").lower()
        text_label = match.group("text").strip("` ").lower()
        target = definitions.get(label) or definitions.get(text_label)
        if target is not None:
            found.append(Link(match.start(), match.end(), match.group("text"), target))
    return found


def external_targets(text: str) -> list[str]:
    """Every http(s) destination, from links and from reference definitions."""
    unfenced, _ = split_fences(text)
    targets = [link.target for link in links_in(unfenced)]
    targets.extend(m.group("target") for m in DEFINITION.finditer(unfenced))
    return sorted({t for t in targets if t.startswith(("http://", "https://"))})


def slug(heading: str) -> str:
    """Return the anchor GitHub gives a heading."""
    text = re.sub(r"!\[[^\]]*\]\([^)]*\)", "", heading)
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"\[([^\]]*)\]\[[^\]]*\]", r"\1", text)
    text = text.replace("`", "").replace("*", "").strip().lower()
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


def anchors(text: str) -> set[str]:
    """Every fragment a link into this document may use."""
    unfenced, _ = split_fences(text)
    seen: dict[str, int] = {}
    found: set[str] = set()
    for line in unfenced.split("\n"):
        match = HEADING.match(line)
        if match is None:
            continue
        base = slug(match.group("text"))
        count = seen.get(base, 0)
        seen[base] = count + 1
        found.add(base if count == 0 else f"{base}-{count}")
    found.update(m.group("name") for m in HTML_ANCHOR.finditer(unfenced))
    return found


def repository_path(target: str) -> str | None:
    """Return the checkout path an absolute repository link names, if any."""
    for kind in ("tree", "blob"):
        prefix = f"{REPOSITORY}/{kind}/master/"
        if target.startswith(prefix):
            return target[len(prefix) :]
    if target == REPOSITORY or target.startswith((REPOSITORY + "#", REPOSITORY + "/#")):
        return "README.md"
    return None


def check_link(
    readme: pathlib.Path, link: Link, root: pathlib.Path
) -> Iterable[Problem]:
    """Report a link whose destination is not in the checkout."""
    target = link.target
    path_part, _, fragment = target.partition("#")
    relative = repository_path(path_part)
    if relative is None:
        if target.startswith(("http://", "https://", "mailto:")):
            return
        destination = readme.parent / path_part if path_part else readme
    else:
        destination = root / relative
    if path_part and not destination.exists():
        yield Problem(readme, f"link target does not exist: {target}")
        return
    if fragment and destination.is_file() and destination.suffix == ".md":
        available = anchors(destination.read_text(encoding="utf-8"))
        if fragment not in available:
            yield Problem(readme, f"no heading or anchor {fragment!r} in {destination}")


def check_introductions(
    readme: pathlib.Path, text: str, known: dict[str, str]
) -> Iterable[Problem]:
    """Report an API named in a section's prose that is not linked there first."""
    unfenced, prose = split_fences(text)
    links = links_in(unfenced)
    starts = [match.start() for match in SECTION.finditer(unfenced)]
    first: dict[tuple[int, str], int] = {}
    for span in CODE_SPAN.finditer(prose):
        head = LEADING_NAME.match(span.group("code").strip())
        if head is None or head.group("name") not in known:
            continue
        section = bisect.bisect_right(starts, span.start())
        first.setdefault((section, head.group("name")), span.start())

    for (_, name), offset in first.items():
        linked = any(link.start <= offset < link.end for link in links)
        line = prose.count("\n", 0, offset) + 1
        if not linked:
            yield Problem(
                readme,
                f"line {line}: the first mention of `{name}` in its section is "
                f"not a link; point it at {REFERENCE}{known[name]}/",
            )
    for name in sorted({name for _, name in first}):
        wanted = f"{REFERENCE}{known[name]}"
        if not any(
            link.target.startswith((wanted + "/", wanted + "-")) for link in links
        ):
            yield Problem(
                readme, f"`{name}` is named in prose but {wanted}/ is never linked"
            )


def rendered_length(lines: list[str]) -> int:
    """Count the characters a reader sees in a block: link text, not targets."""
    text = " ".join(line.strip() for line in lines)
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"\[([^\]]*)\]\[[^\]]*\]", r"\1", text)
    return len(re.sub(r"\s+", " ", text.replace("`", "")))


def check_paragraphs(readme: pathlib.Path, text: str) -> Iterable[Problem]:
    """Report a paragraph or list item too long to scan at a glance.

    Tables, headings, code, HTML, alerts, link definitions and a block of
    links that each end in `·` are indexes or markup, not prose.
    """
    unfenced, _ = split_fences(text)
    lines = unfenced.split("\n")
    index = 0
    while index < len(lines):
        if not lines[index].strip():
            index += 1
            continue
        start = index
        while index < len(lines) and lines[index].strip():
            index += 1
        block = lines[start:index]
        first = block[0].lstrip()
        if first.startswith(("|", "#", "<", ">")) or DEFINITION_LINE.match(first):
            continue
        if sum(line.rstrip().endswith("·") for line in block) >= 2:
            continue
        items: list[tuple[int, list[str]]] = []
        for offset, line in enumerate(block):
            if LIST_ITEM.match(line.lstrip()) or not items:
                items.append((start + offset, []))
            items[-1][1].append(line)
        for line_number, item in items:
            length = rendered_length(item)
            if length > WALL_CHARACTERS:
                yield Problem(
                    readme,
                    f"line {line_number + 1}: a paragraph or list item of "
                    f"{length} characters; split it, or move the detail to docs/",
                )


def check_tool_links(readme: pathlib.Path, text: str) -> Iterable[Problem]:
    """Report a tool name in a table or list that does not link to its page.

    The names are the `[tool-<name>]` definitions the README carries, which its
    generator writes from the server's own registry, so a new tool joins the
    rule when it joins the table.
    """
    tools = {
        match.group("name"): match.group("target")
        for match in TOOL_DEFINITION.finditer(text)
    }
    for name, target in tools.items():
        page = f"{SITE}mcp/tools/{name}/"
        if target != page:
            yield Problem(readme, f"[tool-{name}] points at {target}, not {page}")
    if not tools:
        return

    unfenced, _ = split_fences(text)
    found = links_in(unfenced)
    offset = 0
    for line in unfenced.split("\n"):
        stripped = line.lstrip()
        if stripped.startswith(("|", "- ", "* ")) or re.match(r"\d+\. ", stripped):
            for span in CODE_SPAN.finditer(line):
                name = span.group("code")
                position = offset + span.start()
                linked = any(
                    link.start <= position < link.end
                    and link.target == f"{SITE}mcp/tools/{name}/"
                    for link in found
                )
                if name in tools and not linked:
                    number = unfenced.count("\n", 0, position) + 1
                    yield Problem(
                        readme,
                        f"line {number}: tool `{name}` is not linked to "
                        f"{SITE}mcp/tools/{name}/",
                    )
        offset += len(line) + 1


def check_logo(readme: pathlib.Path, text: str) -> Iterable[Problem]:
    """Report a package README that does not open with the shared logo header."""
    if not text.startswith(LOGO_BEGIN + "\n"):
        yield Problem(readme, f"must open with {LOGO_BEGIN}, the shared logo header")
        return
    end = text.find(LOGO_END)
    if end == -1:
        yield Problem(readme, f"the logo header is never closed by {LOGO_END}")
        return
    block = LOGO_BLOCK.match(text[len(LOGO_BEGIN) : end])
    if block is None:
        yield Problem(
            readme,
            'the logo header must be a <div align="center"> holding a <picture> '
            "with an svg <source> and a 128 by 128 png <img> whose alt text "
            "starts `libtmux for C++`; a <p> wrapper is not accepted",
        )
    else:
        for kind in ("svg", "png"):
            if not (readme.parent / block.group(kind)).is_file():
                yield Problem(
                    readme, f"logo {kind} does not exist: {block.group(kind)}"
                )
    if not text[end + len(LOGO_END) :].startswith('\n\n<div align="center">\n'):
        yield Problem(
            readme,
            f'{LOGO_END} must be followed by a blank line and a <div align="center"> '
            "holding the title",
        )


def check_readme(
    readme: pathlib.Path,
    root: pathlib.Path,
    known: dict[str, str],
    *,
    introductions: bool,
    logo: bool = False,
    paragraphs: bool = False,
) -> list[Problem]:
    """Every problem one README has."""
    text = readme.read_text(encoding="utf-8")
    unfenced, _ = split_fences(text)
    problems: list[Problem] = []
    targets = [link.target for link in links_in(unfenced)]
    targets.extend(m.group("target") for m in DEFINITION.finditer(unfenced))
    for target in dict.fromkeys(targets):
        problems.extend(check_link(readme, Link(0, 0, "", target), root))
    problems.extend(check_tool_links(readme, text))
    if logo:
        problems.extend(check_logo(readme, text))
    if paragraphs:
        problems.extend(check_paragraphs(readme, text))
    if introductions:
        problems.extend(check_introductions(readme, text, known))
    return problems


Fetcher = Callable[[str], int]


def fetch_status(url: str) -> int:
    """Return the final HTTP status of a GET that follows redirects, or 0."""
    request = urllib.request.Request(
        url, headers={"User-Agent": "libtmux-cxx-readme-links"}
    )
    try:
        with urllib.request.urlopen(request, timeout=30) as reply:
            return reply.status
    except urllib.error.HTTPError as error:
        return error.code
    except (urllib.error.URLError, TimeoutError):
        return 0


def check_online(
    readmes: Iterable[pathlib.Path], fetch: Fetcher = fetch_status
) -> list[Problem]:
    """Fetch every external link once and report any that is not a 200."""
    owners: dict[str, pathlib.Path] = {}
    for readme in readmes:
        for target in external_targets(readme.read_text(encoding="utf-8")):
            owners.setdefault(target.partition("#")[0], readme)
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        statuses = dict(zip(owners, pool.map(fetch, owners), strict=True))
    return [
        Problem(owners[url], f"{url} answered {status}, not 200")
        for url, status in statuses.items()
        if status != 200
    ]


# Pages that hold what the READMEs moved out, held to the same links and commands.
EXTRA_PAGES = (
    "docs/runtime.md",
    "docs/windows.md",
    "docs/compatibility.md",
    "docs/mcp-server.md",
    "docs/workspace-cli.md",
    "docs/design/commitments.md",
)


def tracked_readmes(root: pathlib.Path) -> list[pathlib.Path]:
    """Every README the repository tracks, or those under it without a checkout."""
    try:
        listing = subprocess.run(
            ["git", "-C", str(root), "ls-files", "*README.md"],
            capture_output=True,
            text=True,
            check=True,
        )
    except (OSError, subprocess.CalledProcessError):
        skipped = {"build", ".git", "_deps"}
        found = sorted(
            path
            for path in root.rglob("README.md")
            if not skipped.intersection(path.relative_to(root).parts)
        )
        return found + [root / name for name in EXTRA_PAGES if (root / name).is_file()]
    pages = [root / name for name in EXTRA_PAGES if (root / name).is_file()]
    return [root / name for name in listing.stdout.split()] + pages


def main() -> int:
    """Check the READMEs, reporting every broken link and unlinked API."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("readmes", nargs="*", type=pathlib.Path)
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path())
    parser.add_argument(
        "--online",
        action="store_true",
        help="fetch every external link and require a 200",
    )
    arguments = parser.parse_args()

    root = arguments.root.resolve()
    readmes = [path.resolve() for path in arguments.readmes] or tracked_readmes(root)
    known = symbols(root)
    if not known:
        print(f"{root}: no public symbols found in the API pages", file=sys.stderr)
        return 1

    held = {(root / name).resolve() for name in LINKED_READMES}
    logos = {(root / name).resolve() for name in LOGO_READMES}
    problems: list[Problem] = []
    for readme in readmes:
        problems.extend(
            check_readme(
                readme,
                root,
                known,
                introductions=readme in held,
                logo=readme in logos,
                paragraphs=readme in held,
            )
        )
    if arguments.online:
        problems.extend(check_online(readmes))

    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        return 1
    print(f"{len(readmes)} READMEs: every link resolves")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
