"""Render the public C++ surface as one browsable page.

The headers are the source of truth. This walks them, keeps the prose already
written above each declaration, and lays the result out by type. The extractor
is deliberately small, but it follows C++ declaration scopes: function bodies
and private sections are never candidates for the reference.

Run with ``--check`` to verify both the committed page and the parser fixture.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
import typing as t

HEADER_ORDER = [
    "libtmux.hpp",  # first, because it is what a reader includes first
    "server.hpp",
    "capabilities.hpp",
    "entities.hpp",
    "snapshot.hpp",
    "filter_expr.hpp",
    "relations.hpp",
    "cardinality.hpp",
    "delivery.hpp",
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
        preamble=[
            "The full surface targets tmux on POSIX. Windows uses the bounded psmux",
            "preview documented in the project README; unsupported operations fail",
            "before dispatch rather than approximating tmux semantics.",
            "",
        ],
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

TYPE_START = re.compile(
    r"^(?:template\s*<.*?>\s*)?(?P<kind>enum\s+class|class|struct)\s+"
    r"(?P<tail>.+)$"
)
NAMESPACE = re.compile(r"^namespace(?:\s+(?P<name>[A-Za-z_]\w*(?:::\w+)*))?\s*\{")
ACCESS = re.compile(r"^(?P<access>public|private|protected)\s*:\s*$")
FORWARD_TYPE = re.compile(
    r"^(?:template\s*<.*?>\s*)?(?:class|struct|enum\s+class)\s+[^{}]+;$"
)
FUNCTION_NAME = re.compile(r"(?P<name>~?[A-Za-z_]\w*)\s*\(")
OPERATOR_NAME = re.compile(
    r"operator\s*(?P<name>\(\)|\[\]|<=>|==|!=|<=|>=|<<|>>|&&|\|\||"
    r"[+\-*/<>=!&|^~]+|[A-Za-z_][\w:<>,&* ]*)\s*\("
)
LAMBDA_PREFIX = re.compile(
    r"\[[^\]]*\]\s*(?:\([^{}]*\))?\s*(?:mutable\s*)?"
    r"(?:noexcept(?:\([^)]*\))?\s*)?(?:->\s*[^{}]+)?$"
)


class Entry(t.NamedTuple):
    """One public declaration."""

    signature: str
    prose: list[str]
    symbol: str
    category: str
    condition: str = ""


class Section(t.NamedTuple):
    """One public type, or the namespace-level declarations in a header."""

    name: str
    kind: str
    prose: list[str]
    declaration: str
    entries: list[Entry]


class _TypeScope(t.NamedTuple):
    section: Section
    open_depth: int
    public: bool
    opens_on: int


class _NamespaceScope(t.NamedTuple):
    name: str
    open_depth: int
    hidden: bool


class _Candidate(t.NamedTuple):
    text: str
    end_line: int
    body_line: int | None = None


def _collapse(text: str) -> str:
    """Put a declaration that spans lines back onto one."""
    return re.sub(r"\s+", " ", text).strip()


def _source(line: str) -> str:
    """Remove a line comment without treating ``//`` in a literal as one."""
    result: list[str] = []
    quote = ""
    escaped = False
    index = 0
    while index < len(line):
        char = line[index]
        if quote:
            result.append(char)
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = ""
            index += 1
            continue
        if char in {'"', "'"}:
            quote = char
            result.append(char)
            index += 1
            continue
        if char == "/" and index + 1 < len(line) and line[index + 1] == "/":
            break
        result.append(char)
        index += 1
    return "".join(result)


def _code(line: str) -> str:
    """Hide literals after removing comments for structural scanning."""
    source = _source(line)
    result: list[str] = []
    quote = ""
    escaped = False
    for char in source:
        if quote:
            result.append(" ")
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = ""
            continue
        if char in {'"', "'"}:
            quote = char
            result.append(" ")
        else:
            result.append(char)
    return "".join(result)


def _brace_delta(line: str) -> int:
    code = _code(line)
    return code.count("{") - code.count("}")


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


def _function_symbol(signature: str) -> str | None:
    """Return the callable name from a declaration-like prefix."""
    operators = list(OPERATOR_NAME.finditer(signature))
    if operators:
        return "operator" + operators[-1].group("name").strip()
    names = list(FUNCTION_NAME.finditer(signature))
    if not names:
        return None
    ignored = {"decltype", "noexcept", "requires", "sizeof"}
    for match in reversed(names):
        if match.group("name") not in ignored:
            return match.group("name")
    return None


def _looks_like_function(prefix: str) -> bool:
    stripped = _collapse(prefix)
    if stripped.startswith(("using ", "typedef ", "concept ")):
        return False
    return _function_symbol(stripped) is not None


def _strip_constructor_initializer(signature: str) -> str:
    """Drop ``: member_{...}`` from an inline constructor definition."""
    parentheses = 0
    brackets = 0
    for index, char in enumerate(signature):
        if char == "(":
            parentheses += 1
        elif char == ")":
            parentheses = max(0, parentheses - 1)
        elif char == "[":
            brackets += 1
        elif char == "]":
            brackets = max(0, brackets - 1)
        elif (
            char == ":"
            and parentheses == 0
            and brackets == 0
            and (index == 0 or signature[index - 1] != ":")
            and (index + 1 == len(signature) or signature[index + 1] != ":")
        ):
            return signature[:index]
    return signature


def _collect_candidate(lines: list[str], start: int) -> _Candidate | None:
    """Collect one declaration, stopping before an inline function body."""
    text = ""
    structure = ""
    parentheses = 0
    brackets = 0
    braces = 0
    type_kind = ""
    enum_definition = False
    for line_index in range(start, min(len(lines), start + 160)):
        source = _source(lines[line_index]).strip()
        code = _code(source)
        if not code:
            continue
        if text:
            text += " "
            structure += " "
        line_start = len(text)
        text += source
        structure += code
        collapsed = _collapse(structure)
        if not type_kind:
            match = TYPE_START.match(collapsed)
            if match:
                type_kind = match.group("kind")
                enum_definition = type_kind == "enum class"

        for offset, char in enumerate(code):
            absolute = line_start + offset
            if char == "(":
                parentheses += 1
            elif char == ")":
                parentheses = max(0, parentheses - 1)
            elif char == "[":
                brackets += 1
            elif char == "]":
                brackets = max(0, brackets - 1)
            elif char == "{" and parentheses == 0 and brackets == 0:
                if type_kind and braces == 0 and not enum_definition:
                    return _Candidate(
                        _collapse(text[:absolute]), line_index, line_index
                    )
                if braces == 0 and _looks_like_function(structure[:absolute]):
                    signature = _strip_constructor_initializer(text[:absolute])
                    return _Candidate(_collapse(signature), line_index, line_index)
                braces += 1
            elif char == "}" and parentheses == 0 and brackets == 0:
                braces = max(0, braces - 1)
            elif char == ";" and parentheses == 0 and brackets == 0 and braces == 0:
                return _Candidate(_collapse(text[:absolute]), line_index)
    return None


def _type_name(candidate: str) -> tuple[str, str] | None:
    match = TYPE_START.match(candidate)
    if match is None:
        return None
    tail = match.group("tail").split("{", maxsplit=1)[0].strip()
    tail = re.split(r"(?<!:):(?!:)", tail, maxsplit=1)[0].strip()
    tail = re.sub(r"\s+final$", "", tail)
    return match.group("kind"), tail


def _type_declaration(candidate: str, kind: str, name: str) -> str:
    """Keep a public type's template and ``final`` without exposing bases."""
    template = re.match(r"^(template\s*<.*?>\s*)", candidate)
    prefix = template.group(1) if template else ""
    suffix = " final" if re.search(r"\bfinal\b", candidate) else ""
    return _collapse(f"{prefix}{kind} {name}{suffix}")


def _enum_declaration(candidate: str) -> str:
    """Return an enum's declaration without repeating its enumerators."""
    return _collapse(candidate.split("{", maxsplit=1)[0])


def _enum_entries(
    lines: list[str], start: int, end: int, conditions: list[str]
) -> list[Entry]:
    """Split enum members at top-level commas while retaining their prose."""
    entries: list[Entry] = []
    active_conditions = list(conditions)
    inside = False
    parentheses = 0
    brackets = 0
    braces = 0
    current: list[str] = []
    current_line: int | None = None
    current_condition = ""

    def finish() -> None:
        signature = _collapse("".join(current))
        if not signature:
            return
        prose = (
            _prose_above(lines, current_line)
            if current_line is not None and current_line > start
            else []
        )
        entries.append(
            Entry(
                signature,
                prose,
                _field_symbol(signature),
                "enumerator",
                current_condition,
            )
        )

    for line_index in range(start, end + 1):
        stripped = lines[line_index].strip()
        if _update_condition(active_conditions, stripped):
            continue
        source = _source(lines[line_index])
        code = _code(source)
        for offset, char in enumerate(code):
            actual = source[offset]
            if not inside:
                if char == "{":
                    inside = True
                continue

            at_top_level = parentheses == 0 and brackets == 0 and braces == 0
            if at_top_level and char in {",", "}"}:
                finish()
                current.clear()
                current_line = None
                current_condition = ""
                if char == "}":
                    return entries
                continue

            if char == "(":
                parentheses += 1
            elif char == ")":
                parentheses = max(0, parentheses - 1)
            elif char == "[":
                brackets += 1
            elif char == "]":
                brackets = max(0, brackets - 1)
            elif char == "{":
                braces += 1
            elif char == "}":
                braces = max(0, braces - 1)

            if current_line is None and not char.isspace():
                current_line = line_index
                current_condition = _condition(active_conditions)
            current.append(actual)
        if current:
            current.append(" ")
    return entries


def _field_symbol(signature: str) -> str:
    before_initializer = re.split(r"(?<![<>=!])=(?!=)", signature, maxsplit=1)[0]
    before_initializer = before_initializer.split("{", maxsplit=1)[0]
    names = re.findall(r"[A-Za-z_]\w*", before_initializer)
    return names[-1] if names else "declaration"


def _entry(candidate: str, prose: list[str], condition: str, in_type: bool) -> Entry:
    signature = _collapse(candidate)
    concept = re.search(r"\bconcept\s+(?P<name>[A-Za-z_]\w*)", signature)
    if concept:
        return Entry(signature, prose, concept.group("name"), "concept", condition)
    function = _function_symbol(signature) if _looks_like_function(signature) else None
    if function is not None:
        return Entry(signature, prose, function, "function", condition)
    alias = re.search(r"\busing\s+(?P<name>[A-Za-z_]\w*)", signature)
    if alias and "=" in signature:
        return Entry(signature, prose, alias.group("name"), "alias", condition)
    inherited = re.search(r"\busing\s+[^;]*::(?P<name>[A-Za-z_]\w*)$", signature)
    if inherited:
        return Entry(signature, prose, inherited.group("name"), "alias", condition)
    if alias:
        return Entry(signature, prose, alias.group("name"), "alias", condition)
    symbol = _field_symbol(signature)
    category = "field" if in_type else "constant"
    return Entry(signature, prose, symbol, category, condition)


def _elide_lambda_bodies(signature: str) -> str:
    """Replace executable lambda bodies while preserving initializer shape."""
    code = _code(signature)
    spans: list[tuple[int, int]] = []
    index = 0
    while index < len(code):
        if code[index] != "{" or LAMBDA_PREFIX.search(code[:index]) is None:
            index += 1
            continue
        depth = 1
        end = index + 1
        while end < len(code) and depth:
            if code[end] == "{":
                depth += 1
            elif code[end] == "}":
                depth -= 1
            end += 1
        if depth == 0:
            spans.append((index + 1, end - 1))
            index = end
        else:
            break
    for start, end in reversed(spans):
        signature = (
            signature[:start] + " /* implementation omitted */ " + signature[end:]
        )
    return signature


def _elide_macro_body(signature: str) -> str:
    """Keep a statement macro's call shape without publishing its body."""
    lines = signature.splitlines()
    body = next(
        (index for index, line in enumerate(lines) if line.strip().startswith("do {")),
        None,
    )
    if body is None:
        return signature
    declaration = " ".join(line.rstrip("\\").strip() for line in lines[:body])
    return _collapse(declaration) + " /* implementation omitted */"


def _condition(stack: list[str]) -> str:
    """Describe the active preprocessor branches."""
    return " and ".join(item for item in stack if item)


def _update_condition(stack: list[str], stripped: str) -> bool:
    """Update conditional-compilation state and report handled directives."""
    if stripped.startswith("#if "):
        stack.append(stripped.removeprefix("#if ").strip())
        return True
    if stripped.startswith("#ifdef "):
        stack.append("defined(" + stripped.removeprefix("#ifdef ").strip() + ")")
        return True
    if stripped.startswith("#ifndef "):
        stack.append("!defined(" + stripped.removeprefix("#ifndef ").strip() + ")")
        return True
    if stripped.startswith("#elif ") and stack:
        stack[-1] = stripped.removeprefix("#elif ").strip()
        return True
    if stripped == "#else" and stack:
        stack[-1] = "!(" + stack[-1] + ")"
        return True
    if stripped == "#endif" and stack:
        stack.pop()
        return True
    return False


def _macro(lines: list[str], start: int) -> _Candidate:
    """Collect one possibly continued preprocessor macro."""
    parts = [lines[start].strip()]
    end = start
    while parts[-1].endswith("\\") and end + 1 < len(lines):
        end += 1
        parts.append(lines[end].strip())
    return _Candidate("\n".join(parts), end)


def _macro_entry(lines: list[str], start: int, condition: str) -> Entry:
    """Build an entry for one public macro."""
    candidate = _macro(lines, start)
    match = re.match(r"#define\s+([A-Za-z_]\w*)", candidate.text)
    symbol = match.group(1) if match else "macro"
    return Entry(candidate.text, _prose_above(lines, start), symbol, "macro", condition)


def _namespace_prefix(scopes: list[_NamespaceScope]) -> str:
    """Return public namespaces below the library's root namespace."""
    names = [scope.name for scope in scopes if scope.name]
    if names and names[0] in {"libtmux", "libtmux::test"}:
        names.pop(0)
    return "::".join(names)


def read_header(path: pathlib.Path) -> tuple[list[str], list[Section]]:
    """Return a header's opening prose and scope-checked public declarations."""
    lines = path.read_text(encoding="utf-8").splitlines()

    overview: list[str] = []
    for line in lines[1:]:
        stripped = line.strip()
        if stripped.startswith("//"):
            overview.append(stripped.removeprefix("//").strip())
        elif overview:
            break

    free = Section("Free symbols", "namespace", [], "", [])
    sections: list[Section] = []
    type_scopes: list[_TypeScope] = []
    namespace_scopes: list[_NamespaceScope] = []
    pending_types: dict[int, _TypeScope] = {}
    conditions: list[str] = []
    depth = 0
    consumed_through = -1

    for index, line in enumerate(lines):
        stripped = line.strip()
        code = _code(line).strip()

        if _update_condition(conditions, stripped):
            continue

        if namespace := NAMESPACE.match(code):
            name = namespace.group("name") or ""
            namespace_scopes.append(
                _NamespaceScope(
                    name=name,
                    open_depth=depth + 1,
                    hidden=name == "detail"
                    or bool(namespace_scopes and namespace_scopes[-1].hidden),
                )
            )

        hidden = any(scope.hidden for scope in namespace_scopes)
        expected_depth = (
            type_scopes[-1].open_depth
            if type_scopes
            else namespace_scopes[-1].open_depth
            if namespace_scopes
            else 0
        )
        direct_public = not type_scopes or type_scopes[-1].public

        if not hidden and depth == expected_depth and index > consumed_through:
            if access := ACCESS.match(code):
                if type_scopes:
                    current = type_scopes[-1]
                    type_scopes[-1] = current._replace(
                        public=access.group("access") == "public"
                    )
            elif direct_public and stripped.startswith("#define "):
                free.entries.append(_macro_entry(lines, index, _condition(conditions)))
                consumed_through = _macro(lines, index).end_line
            elif direct_public and (
                code
                and not code.startswith(("#", "}", "namespace ", "LIBTMUX_NAMESPACE"))
            ):
                candidate = _collect_candidate(lines, index)
                if candidate is not None:
                    public_type = _type_name(candidate.text)
                    if public_type is not None:
                        kind, name = public_type
                        is_definition = candidate.body_line is not None or (
                            kind == "enum class" and "{" in _code(candidate.text)
                        )
                        if is_definition:
                            qualified_parent = (
                                type_scopes[-1].section.name if type_scopes else ""
                            )
                            qualified = (
                                f"{qualified_parent}::{name}"
                                if qualified_parent
                                else name
                            )
                            section = Section(
                                name=qualified,
                                kind=kind,
                                prose=_prose_above(lines, index),
                                declaration=(
                                    _enum_declaration(candidate.text)
                                    if kind == "enum class"
                                    else _type_declaration(candidate.text, kind, name)
                                ),
                                entries=(
                                    _enum_entries(
                                        lines,
                                        index,
                                        candidate.end_line,
                                        conditions,
                                    )
                                    if kind == "enum class"
                                    else []
                                ),
                            )
                            sections.append(section)
                            if candidate.body_line is not None:
                                pending_types[candidate.body_line] = _TypeScope(
                                    section=section,
                                    open_depth=depth + 1,
                                    public=kind == "struct",
                                    opens_on=candidate.body_line,
                                )
                    elif not FORWARD_TYPE.match(
                        candidate.text + ";"
                    ) and not candidate.text.startswith(("friend ", "static_assert")):
                        target = type_scopes[-1].section if type_scopes else free
                        found = _entry(
                            candidate.text,
                            _prose_above(lines, index),
                            _condition(conditions),
                            bool(type_scopes),
                        )
                        prefix = _namespace_prefix(namespace_scopes)
                        if not type_scopes and prefix:
                            found = found._replace(symbol=f"{prefix}::{found.symbol}")
                        target.entries.append(found)
                    consumed_through = candidate.end_line

        depth += _brace_delta(line)

        if index in pending_types and depth >= pending_types[index].open_depth:
            type_scopes.append(pending_types[index])

        while (
            type_scopes
            and index >= type_scopes[-1].opens_on
            and depth < type_scopes[-1].open_depth
        ):
            type_scopes.pop()
        while namespace_scopes and depth < namespace_scopes[-1].open_depth:
            namespace_scopes.pop()

    if free.entries:
        sections.append(free)
    return overview, sections


def _slug(text: str) -> str:
    slug = re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")
    return slug or "symbol"


def _anchors(header: str, sections: list[Section]) -> dict[int, str]:
    """Assign stable, unique anchors to public types and symbols."""
    anchors: dict[int, str] = {}
    used: dict[str, int] = {}
    for section in sections:
        base = _slug(f"{header}-{section.name}")
        used[base] = used.get(base, 0) + 1
        anchors[id(section)] = base if used[base] == 1 else f"{base}-{used[base]}"
        for entry in section.entries:
            entry_base = _slug(f"{header}-{section.name}-{entry.symbol}")
            used[entry_base] = used.get(entry_base, 0) + 1
            anchors[id(entry)] = (
                entry_base
                if used[entry_base] == 1
                else f"{entry_base}-{used[entry_base]}"
            )
    return anchors


def _render_header(path: pathlib.Path, heading: str) -> list[str]:
    """Render one header and its symbol navigation."""
    overview, sections = read_header(path)
    anchors = _anchors(heading, sections)
    out = [f'<a id="{_slug(heading)}"></a>', f"## `{heading}`", ""]
    if overview:
        out.extend([" ".join(overview).strip(), ""])
    if sections:
        out.append("**Symbols:**")
        out.append("")
        for section in sections:
            out.append(f"- [`{section.name}`](#{anchors[id(section)]})")
            out.extend(
                (
                    f"  - [`{section.name}::{entry.symbol}`](#{anchors[id(entry)]})"
                    if section.name != "Free symbols"
                    else f"  - [`{entry.symbol}`](#{anchors[id(entry)]})"
                )
                for entry in section.entries
            )
        out.append("")

    for section in sections:
        out.extend(
            [
                f'<a id="{anchors[id(section)]}"></a>',
                f"### `{section.name}`",
                "",
            ]
        )
        if section.prose:
            out.extend([" ".join(section.prose).strip(), ""])
        if section.declaration:
            out.extend([f"```cpp\n{section.declaration};\n```", ""])
        for entry in section.entries:
            label = (
                f"{section.name}::{entry.symbol}"
                if section.name != "Free symbols"
                else entry.symbol
            )
            if entry.category == "enumerator":
                out.extend(
                    [
                        f'<a id="{anchors[id(entry)]}"></a>',
                        f"#### `{label}` — `{entry.signature},`",
                        "",
                    ]
                )
                if entry.condition:
                    out.append(f"Available when `{entry.condition}`.")
                if entry.prose:
                    out.extend([" ".join(entry.prose).strip(), ""])
                continue
            terminator = "" if entry.category == "macro" else ";"
            signature = _elide_lambda_bodies(entry.signature)
            if entry.category == "macro":
                signature = _elide_macro_body(signature)
            out.extend(
                [
                    f'<a id="{anchors[id(entry)]}"></a>',
                    f"#### `{label}`",
                    "",
                    f"```cpp\n{signature}{terminator}\n```",
                ]
            )
            if entry.condition:
                out.append(f"Available when `{entry.condition}`.")
            if entry.prose:
                out.append(" ".join(entry.prose).strip())
            out.append("")
    return out


def render(root: pathlib.Path, page: Page) -> str:
    """Render one page, refusing unplaced public headers."""
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
        "## Headers",
        "",
    ]
    for name in page.headers:
        if not (root / name).exists():
            continue
        heading = f"{page.include_prefix}{name}"
        out.append(f"- [`{heading}`](#{_slug(heading)})")
    out.append("")
    for name in page.headers:
        path = root / name
        if path.exists():
            out.extend(_render_header(path, f"{page.include_prefix}{name}"))
    return "\n".join(out).rstrip() + "\n"


def _check_fixture(script: pathlib.Path) -> str | None:
    """Return an error when the focused parser fixture has drifted."""
    fixture = script.with_name("fixtures") / "api_index.hpp"
    expected = script.with_name("fixtures") / "api_index.expected.md"
    if not fixture.exists() or not expected.exists():
        return "API reference parser fixture is missing"
    rendered = (
        "\n".join(_render_header(fixture, "fixture/api_index.hpp")).rstrip() + "\n"
    )
    wanted = expected.read_text(encoding="utf-8")
    if rendered != wanted:
        return (
            f"{expected} is out of date; regenerate it from the focused parser fixture"
        )
    return None


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
        if fixture_error := _check_fixture(pathlib.Path(__file__)):
            sys.stderr.write(fixture_error + "\n")
            return 1
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
